local log = require("log")
local fiber = require('fiber')
local tnt_kafka = require("tnt-kafka.tntkafka")

local Consumer = {}

Consumer.__index = Consumer

function Consumer.create(config)
    if config == nil then
        return nil, "config must not be nil"
    end

    local consumer, err = tnt_kafka.create_consumer(config)
    if err ~= nil then
        return nil, err
    end

    local new = {
        config = config,
        _consumer = consumer,
        _output_ch = fiber.channel(10000),
    }
    setmetatable(new, Consumer)

    new._poll_fiber = fiber.create(function()
        new:_poll()
    end)

    new._poll_msg_fiber = fiber.create(function()
        new:_poll_msg()
    end)

    return new, nil
end

function Consumer:_poll()
    local err
    while true do
        err = self._consumer:poll()
        if err ~= nil then
            log.error(err)
        end
    end
end

jit.off(Consumer._poll)

function Consumer:_poll_msg()
    local msgs
    while true do
        msgs = self._consumer:poll_msg(100)
        if #msgs > 0 then
            for _, msg in ipairs(msgs) do
                self._output_ch:put(msg)
            end
            fiber.yield()
        else
            -- throtling poll
            fiber.sleep(0.01)
        end
    end
end

jit.off(Consumer._poll_msg)

function Consumer:close()
    self._poll_msg_fiber:cancel()
    self._poll_fiber:cancel()
    self._output_ch:close()

    local ok, err = self._consumer:close()
    self._consumer = nil

    return err
end

function Consumer:subscribe(topics)
    return self._consumer:subscribe(topics)
end

function Consumer:unsubscribe(topics)
    return self._consumer:unsubscribe(topics)
end

function Consumer:output()
    return self._output_ch
end

function Consumer:store_offset(message)
    return self._consumer:store_offset(message)
end

local Producer = {}

Producer.__index = Producer

function Producer.create(config)
    if config == nil then
        return nil, "config must not be nil"
    end

    local producer, err = tnt_kafka.create_producer(config)
    if err ~= nil then
        return nil, err
    end

    local new = {
        config = config,
        _counter = 0,
        _delivery_map = {},
        _producer = producer,
    }
    setmetatable(new, Producer)

    new._poll_fiber = fiber.create(function()
        new:_poll()
    end)

    new._msg_delivery_poll_fiber = fiber.create(function()
        new:_msg_delivery_poll()
    end)

    return new, nil
end

function Producer:_poll()
    local err
    while true do
        err = self._producer:poll()
        if err ~= nil then
            log.error(err)
        end
    end
end

jit.off(Producer._poll)

function Producer:_msg_delivery_poll()
    local count, err
    while true do
        local count, err
        while true do
            count, err = self._producer:msg_delivery_poll(100)
            if err ~= nil then
                log.error(err)
                -- throtling poll
                fiber.sleep(0.01)
            elseif count > 0 then
                fiber.yield()
            else
                -- throtling poll
                fiber.sleep(0.01)
            end
        end
    end
end

jit.off(Producer._msg_delivery_poll)

function Producer:produce_async(msg)
    local err = self._producer:produce(msg)
    return err
end

local function dr_callback_factory(delivery_chan)
    return function(err)
        delivery_chan:put(err)
    end
end

function Producer:produce(msg)
    local delivery_chan = fiber.channel(1)

    msg.dr_callback = dr_callback_factory(delivery_chan)

    local err = self._producer:produce(msg)
    if err == nil then
        err = delivery_chan:get()
    end

    return err
end

function Producer:close()
    self._poll_fiber:cancel()
    self._msg_delivery_poll_fiber:cancel()

    local ok, err = self._producer:close()
    self._producer = nil

    return err
end

return {
    Consumer = Consumer,
    Producer = Producer,
}
