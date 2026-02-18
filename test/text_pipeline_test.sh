#!/bin/bash
# test_pipeline.sh - Real pipeline test

HOST="127.0.0.1"
PORT="2000"

echo "=== Real pipeline batch test ==="

# 1. Create a temporary file with all commands
cat > /tmp/kv_commands.txt << EOF
HSET test_key test_value
HGET test_key
HEXIST test_key
HMOD test_key new_value
HGET test_key
HDEL test_key
HEXIST test_key
HGET not_exist_key
HDEL not_exist_key
HSET key1 val1
HSET key2 val2
HSET key3 val3
HGET key1
HGET key2
HGET key3
EOF

# 2. Send all commands in one connection
echo "Sending all commands (pipeline mode)..."
(
    while read cmd; do
        echo -ne "$cmd\r\n"
        sleep 0.01  # tiny delay to ensure order
    done < /tmp/kv_commands.txt
    sleep 0.5  # wait for all responses
) | nc $HOST $PORT

echo -e "\n=== Test completed ==="
