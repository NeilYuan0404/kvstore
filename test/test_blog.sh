#!/bin/bash

HOST=${1:-127.0.0.1}
PORT=${2:-6379}
KEY="kvs-persist"
FIELD="content"
FILE="/home/neil/MyProjects/new-kvstore/doc/persist.md"

# 获取文件大小
SIZE=$(stat -c%s "$FILE")

# 构造并发送 RESP 命令
{
    # *3\r\n$4\r\nHSET\r\n
    printf "*3\r\n\$4\r\nHSET\r\n"
    
    # key
    printf "\$%d\r\n%s\r\n" ${#KEY} "$KEY"
    
    # field
    printf "\$%d\r\n%s\r\n" ${#FIELD} "$FIELD"
    
    # value (文件内容)
    printf "\$%d\r\n" $SIZE
    cat "$FILE"
    printf "\r\n"
} | nc "$HOST" "$PORT"

echo "插入完成"
