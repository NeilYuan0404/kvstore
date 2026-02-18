#!/bin/bash
# Script to truncate AOF and RDB files (clear content without deleting files)

# Default data directory (matches kvs_persist_init default path)
DATA_DIR="../data"

# Use argument as data directory if provided
if [ $# -eq 1 ]; then
    DATA_DIR="$1"
fi

# Check if directory exists
if [ ! -d "$DATA_DIR" ]; then
    echo "Error: Data directory '$DATA_DIR' does not exist."
    exit 1
fi

# Clear AOF file
AOF_FILE="$DATA_DIR/kvstore.aof"
if [ -f "$AOF_FILE" ]; then
    > "$AOF_FILE"
    echo "Cleared AOF file: $AOF_FILE"
else
    echo "AOF file not found, skipping: $AOF_FILE"
fi

# Clear RDB file
RDB_FILE="$DATA_DIR/kvstore.rdb"
if [ -f "$RDB_FILE" ]; then
    > "$RDB_FILE"
    echo "Cleared RDB file: $RDB_FILE"
else
    echo "RDB file not found, skipping: $RDB_FILE"
fi

echo "Done."
