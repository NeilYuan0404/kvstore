#!/bin/bash
# Quick batch test template
HOST="127.0.0.1"
PORT="2000"

# Define commands to test
declare -a commands=(
    "# Test basic hash operations"
    "HSET test_key test_value"
    "HGET test_key"
    "HEXIST test_key"
    "HMOD test_key new_value"
    "HGET test_key"
    "HDEL test_key"
    "HEXIST test_key"
    
    "# Test error cases"
    "HGET not_exist_key"
    "HDEL not_exist_key"
    
    "# Test multiple keys"
    "HSET key1 val1"
    "HSET key2 val2"
    "HSET key3 val3"
    "HGET key1"
    "HGET key2"
    "HGET key3"
)

echo "Connecting to $HOST:$PORT ..."

# Execute all commands
for cmd in "${commands[@]}"; do
    if [[ "$cmd" == \#* ]]; then
        # This is a comment, display it
        echo ""
        echo "== $(echo $cmd | cut -c 3-) =="
    else
        echo "→ $cmd"
        echo -ne "$cmd\r\n" | nc -w 2 $HOST $PORT 2>/dev/null || echo "No response"
        sleep 0.05
    fi
done

echo "Test completed!"
