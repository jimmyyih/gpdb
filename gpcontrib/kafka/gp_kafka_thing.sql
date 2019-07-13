CREATE FUNCTION kafka_send_message(text) RETURNS bool
LANGUAGE C VOLATILE AS '$libdir/gp_kafka_thing.so', 'kafka_send_message';
