#!/bin/bash
# test_resp_single.sh
HOST="127.0.0.1"
PORT="2000"

echo "=== Testing RESP format support ==="
echo "Server: $HOST:$PORT"
echo

# Test HSET command (RESP format)
echo "1. Testing HSET command in RESP format:"
echo -ne "*3\r\n\$4\r\nHSET\r\n\$7\r\ntestkey\r\n\$9\r\ntestvalue\r\n" | nc -w 2 $HOST $PORT
echo

# Test HGET command (RESP format)
echo "2. Testing HGET command in RESP format:"
echo -ne "*2\r\n\$4\r\nHGET\r\n\$7\r\ntestkey\r\n" | nc -w 2 $HOST $PORT
echo

# Test HDEL command (RESP format)
echo "3. Testing HDEL command in RESP format:"
echo -ne "*2\r\n\$4\r\nHDEL\r\n\$7\r\ntestkey\r\n" | nc -w 2 $HOST $PORT
echo

echo "=== Test completed ==="
