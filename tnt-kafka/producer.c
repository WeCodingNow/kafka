#include <stdlib.h>
#include <errno.h>

#include <librdkafka/rdkafka.h>
#include <tarantool/module.h>

#include <common.h>
#include <callbacks.h>
#include <queue.h>

#include "producer.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Producer
 */

producer_topics_t *
new_producer_topics(int32_t capacity) {
    rd_kafka_topic_t **elements;
    elements = malloc(sizeof(rd_kafka_topic_t *) * capacity);

    producer_topics_t *topics;
    topics = malloc(sizeof(producer_topics_t));
    topics->capacity = capacity;
    topics->count = 0;
    topics->elements = elements;

    return topics;
}

int
add_producer_topics(producer_topics_t *topics, rd_kafka_topic_t *element) {
    if (topics->count >= topics->capacity) {
        rd_kafka_topic_t **new_elements = realloc(topics->elements, sizeof(rd_kafka_topic_t *) * topics->capacity * 2);
        if (new_elements == NULL) {
            printf("realloc failed to relloc rd_kafka_topic_t array.");
            return 1;
        }
        topics->elements = new_elements;
        topics->capacity *= 2;
    }
    topics->elements[topics->count++] = element;
    return 0;
}

rd_kafka_topic_t *
find_producer_topic_by_name(producer_topics_t *topics, const char *name) {
    rd_kafka_topic_t *topic;
    for (int i = 0; i < topics->count; i++) {
        topic = topics->elements[i];
        if (strcmp(rd_kafka_topic_name(topic), name) == 0) {
            return topic;
        }
    }
    return NULL;
}

void
destroy_producer_topics(producer_topics_t *topics) {
    rd_kafka_topic_t **topic_p;
    rd_kafka_topic_t **end = topics->elements + topics->count;
    for (topic_p = topics->elements; topic_p < end; topic_p++) {
        rd_kafka_topic_destroy(*topic_p);
    }

    free(topics->elements);
    free(topics);
}

queue_element_t *
new_queue_element(int dr_callback, int err) {
    queue_element_t *element;
    element = malloc(sizeof(queue_element_t));
    element->dr_callback = dr_callback;
    element->err = err;
    return element;
}

void
destroy_queue_element(queue_element_t *element) {
    free(element);
}

static inline producer_t *
lua_check_producer(struct lua_State *L, int index) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, index, producer_label);
    if (producer_p == NULL || *producer_p == NULL)
        luaL_error(L, "Kafka consumer fatal error: failed to retrieve producer from lua stack!");
    return *producer_p;
}

int
lua_producer_tostring(struct lua_State *L) {
    producer_t *producer = lua_check_producer(L, 1);
    lua_pushfstring(L, "Kafka Producer: %p", producer);
    return 1;
}

static ssize_t
producer_poll(va_list args) {
    rd_kafka_t *rd_producer = va_arg(args, rd_kafka_t *);
    rd_kafka_poll(rd_producer, 1000);
    return 0;
}

int
lua_producer_poll(struct lua_State *L) {
    if (lua_gettop(L) != 1)
        luaL_error(L, "Usage: err = producer:poll()");

    producer_t *producer = lua_check_producer(L, 1);
    if (coio_call(producer_poll, producer->rd_producer) == -1) {
        lua_pushstring(L, "unexpected error on producer poll");
        return 1;
    }
    return 0;
}

int
lua_producer_msg_delivery_poll(struct lua_State *L) {
    if (lua_gettop(L) != 2)
        luaL_error(L, "Usage: count, err = producer:msg_delivery_poll(events_limit)");

    producer_t *producer = lua_check_producer(L, 1);

    int events_limit = lua_tonumber(L, 2);
    int callbacks_count = 0;
    char *err_str = NULL;
    queue_element_t *element = NULL;

    pthread_mutex_lock(&producer->delivery_queue->lock);

    while (events_limit > callbacks_count) {
        element = queue_lockfree_pop(producer->delivery_queue);
        if (element == NULL) {
            break;
        }
        callbacks_count += 1;
        lua_rawgeti(L, LUA_REGISTRYINDEX, element->dr_callback);
        if (element->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            lua_pushstring(L, (char *)rd_kafka_err2str(element->err));
        } else {
            lua_pushnil(L);
        }
        /* do the call (1 arguments, 0 result) */
        if (lua_pcall(L, 1, 0, 0) != 0) {
            err_str = (char *)lua_tostring(L, -1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, element->dr_callback);
        destroy_queue_element(element);
        if (err_str != NULL) {
            break;
        }
    }

    pthread_mutex_unlock(&producer->delivery_queue->lock);

    lua_pushnumber(L, (double)callbacks_count);
    if (err_str != NULL) {
        int fail = safe_pushstring(L, err_str);
        if (fail) {
            return lua_push_error(L);
        }
    } else {
        lua_pushnil(L);
    }
    return 2;
}

int
lua_producer_produce(struct lua_State *L) {
    if (lua_gettop(L) != 2 || !lua_istable(L, 2))
        luaL_error(L, "Usage: err = producer:produce(msg)");

    lua_pushstring(L, "topic");
    lua_gettable(L, -2 );
    const char *topic = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (topic == NULL) {
        int fail = safe_pushstring(L, "producer message must contains non nil 'topic' key");
        return fail ? lua_push_error(L): 1;
    }

    lua_pushstring(L, "key");
    lua_gettable(L, -2 );
    // rd_kafka will copy key so no need to worry about this cast
    char *key = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);

    size_t key_len = key != NULL ? strlen(key) : 0;

    lua_pushstring(L, "value");
    lua_gettable(L, -2 );
    // rd_kafka will copy value so no need to worry about this cast
    char *value = (char *)lua_tostring(L, -1);
    lua_pop(L, 1);

    size_t value_len = value != NULL ? strlen(value) : 0;

    if (key == NULL && value == NULL) {
        int fail = safe_pushstring(L, "producer message must contains non nil key or value");
        return fail ? lua_push_error(L): 1;
    }

    // create delivery callback queue if got msg id
    queue_element_t *element = NULL;
    lua_pushstring(L, "dr_callback");
    lua_gettable(L, -2 );
    if (lua_isfunction(L, -1)) {
        element = new_queue_element(luaL_ref(L, LUA_REGISTRYINDEX), RD_KAFKA_RESP_ERR_NO_ERROR);
        if (element == NULL) {
            int fail = safe_pushstring(L, "failed to create callback message");
            return fail ? lua_push_error(L): 1;
        }
    } else {
        lua_pop(L, 1);
    }

    // pop msg
    lua_pop(L, 1);

    producer_t *producer = lua_check_producer(L, 1);
    rd_kafka_topic_t *rd_topic = find_producer_topic_by_name(producer->topics, topic);
    if (rd_topic == NULL) {
        rd_topic = rd_kafka_topic_new(producer->rd_producer, topic, NULL);
        if (rd_topic == NULL) {
            const char *const_err_str = rd_kafka_err2str(rd_kafka_errno2err(errno));
            char err_str[512];
            strcpy(err_str, const_err_str);
            int fail = safe_pushstring(L, err_str);
            return fail ? lua_push_error(L): 1;
        }
        if (add_producer_topics(producer->topics, rd_topic) != 0) {
            int fail = safe_pushstring(L, "Unexpected error: failed to add new topic to topic list!");
            return fail ? lua_push_error(L): 1;
        }
    }

    if (rd_kafka_produce(rd_topic, -1, RD_KAFKA_MSG_F_COPY, value, value_len, key, key_len, element) == -1) {
        const char *const_err_str = rd_kafka_err2str(rd_kafka_errno2err(errno));
        char err_str[512];
        strcpy(err_str, const_err_str);
        int fail = safe_pushstring(L, err_str);
        return fail ? lua_push_error(L): 1;
    }
    return 0;
}

static ssize_t
producer_flush(va_list args) {
    rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
    rd_kafka_t *rd_producer = va_arg(args, rd_kafka_t *);
    while (true) {
        err = rd_kafka_flush(rd_producer, 1000);
        if (err != RD_KAFKA_RESP_ERR__TIMED_OUT) {
            break;
        }
    }
    return 0;
}

static rd_kafka_resp_err_t
producer_close(producer_t *producer) {
    rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

    if (producer->rd_producer != NULL) {
        coio_call(producer_flush, producer->rd_producer);
    }

    if (producer->topics != NULL) {
        destroy_producer_topics(producer->topics);
    }

    if (producer->delivery_queue != NULL) {
        destroy_queue(producer->delivery_queue);
    }

    if (producer->rd_producer != NULL) {
        // FIXME: if instance of consumer exists then kafka_destroy always hangs forever
        /* Destroy handle */
//        coio_call(kafka_destroy, producer->rd_producer);
    }

    free(producer);
    return err;
}

int
lua_producer_close(struct lua_State *L) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, 1, producer_label);
    if (producer_p == NULL || *producer_p == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }

    rd_kafka_resp_err_t err = producer_close(*producer_p);
    if (err) {
        lua_pushboolean(L, 1);

        const char *const_err_str = rd_kafka_err2str(err);
        char err_str[512];
        strcpy(err_str, const_err_str);
        int fail = safe_pushstring(L, err_str);
        return fail ? lua_push_error(L): 2;
    }

    *producer_p = NULL;
    lua_pushboolean(L, 1);
    return 1;
}

int
lua_producer_gc(struct lua_State *L) {
    producer_t **producer_p = (producer_t **)luaL_checkudata(L, 1, producer_label);
    if (producer_p && *producer_p) {
        producer_close(*producer_p);
    }
    if (producer_p)
        *producer_p = NULL;
    return 0;
}

void
msg_delivery_callback(rd_kafka_t *UNUSED(producer), const rd_kafka_message_t *msg, void *opaque) {
    if (msg->_private != NULL) {
        queue_element_t *element = msg->_private;
        queue_t *queue = opaque;
        if (element != NULL) {
            if (msg->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
                element->err = msg->err;
            }
            queue_push(queue, element);
        }
    }
}

int
lua_create_producer(struct lua_State *L) {
    if (lua_gettop(L) != 1 || !lua_istable(L, 1))
        luaL_error(L, "Usage: producer, err = create_producer(conf)");

    lua_pushstring(L, "brokers");
    lua_gettable(L, -2 );
    const char *brokers = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (brokers == NULL) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, "producer config table must have non nil key 'brokers' which contains string");
        return fail ? lua_push_error(L): 2;
    }

    char errstr[512];

    rd_kafka_conf_t *rd_config = rd_kafka_conf_new();

    queue_t *delivery_queue = new_queue();
    // queue now accessible from callback
    rd_kafka_conf_set_opaque(rd_config, delivery_queue);

    rd_kafka_conf_set_dr_msg_cb(rd_config, msg_delivery_callback);

    // enabling delivering events
//    rd_kafka_conf_set_events(rd_config, RD_KAFKA_EVENT_STATS | RD_KAFKA_EVENT_DR);

    lua_pushstring(L, "options");
    lua_gettable(L, -2 );
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        // stack now contains: -1 => nil; -2 => table
        while (lua_next(L, -2)) {
            // stack now contains: -1 => value; -2 => key; -3 => table
            if (!(lua_isstring(L, -1)) || !(lua_isstring(L, -2))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, "producer config options must contains only string keys and string values");
                return fail ? lua_push_error(L): 2;
            }

            const char *value = lua_tostring(L, -1);
            const char *key = lua_tostring(L, -2);
            if (rd_kafka_conf_set(rd_config, key, value, errstr, sizeof(errstr))) {
                lua_pushnil(L);
                int fail = safe_pushstring(L, errstr);
                return fail ? lua_push_error(L): 2;
            }

            // pop value, leaving original key
            lua_pop(L, 1);
            // stack now contains: -1 => key; -2 => table
        }
        // stack now contains: -1 => table
    }
    lua_pop(L, 1);

    rd_kafka_t *rd_producer;
    if (!(rd_producer = rd_kafka_new(RD_KAFKA_PRODUCER, rd_config, errstr, sizeof(errstr)))) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, errstr);
        return fail ? lua_push_error(L): 2;
    }

    if (rd_kafka_brokers_add(rd_producer, brokers) == 0) {
        lua_pushnil(L);
        int fail = safe_pushstring(L, "No valid brokers specified");
        return fail ? lua_push_error(L): 2;
    }

    producer_t *producer;
    producer = malloc(sizeof(producer_t));
    producer->rd_producer = rd_producer;
    producer->topics = new_producer_topics(256);
    producer->delivery_queue = delivery_queue;

    producer_t **producer_p = (producer_t **)lua_newuserdata(L, sizeof(producer));
    *producer_p = producer;

    luaL_getmetatable(L, producer_label);
    lua_setmetatable(L, -2);
    return 1;
}
