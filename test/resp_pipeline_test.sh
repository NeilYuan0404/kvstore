#!/bin/bash

HOST="127.0.0.1"
PORT="2000"

echo "=== Testing RESP pipeline ==="

# 确保所有输出都是纯 RESP 格式
{
    # 批量插入 
    printf "*3\r\n\$4\r\nHSET\r\n\$6\r\nbatch1\r\n\$6\r\nvalue1\r\n"
    printf "*3\r\n\$4\r\nHSET\r\n\$6\r\nbatch2\r\n\$6\r\nvalue2\r\n"
    printf "*3\r\n\$4\r\nHSET\r\n\$6\r\nbatch3\r\n\$6\r\nvalue3\r\n"
    
    # 批量获取
    printf "*2\r\n\$4\r\nHGET\r\n\$6\r\nbatch1\r\n"
    printf "*2\r\n\$4\r\nHGET\r\n\$6\r\nbatch2\r\n"
    printf "*2\r\n\$4\r\nHGET\r\n\$6\r\nbatch3\r\n"
    
    # 批量删除和检查
    printf "*2\r\n\$4\r\nHDEL\r\n\$6\r\nbatch1\r\n"
    printf "*2\r\n\$6\r\nHEXIST\r\n\$6\r\nbatch1\r\n"
    printf "*2\r\n\$6\r\nHEXIST\r\n\$6\r\nbatch2\r\n"
} | nc "$HOST" "$PORT"

echo -e "\n=== Test completed ==="
