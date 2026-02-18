## 一、内容概要

在本实现当中，增量持久化采用了RESP格式（Redis兼容）的方案，存量持久化采用了二进制文本存储key、key_len、value、value_len的方案。


## 二、增量持久化

增量持久化，即当用户输入SET，MOD，DEL指令的时候，kv存储器对数据结构进行写入命令的存储。采用RESP的方案

如果直接存储指令如：

SET 123 Alice
SET 456 Bob

以telnet为例，客户端的指令如下：

SET 123 Alice\r\nSET 456 Bob\r\n

然而，由于TCP分包的现象存在，服务端接受指令可能出现不完整的情况，比如当\r\n 被拆开:

包1: "SET 123 Alice\r"
包2: "\nSET 456 Bob\r"
包3: "\n"

亦或者key、value被拆开，系统将难以处理。

包1: "SET 123 Ali"      ← "Alice"被拆开！
包2: "ce\r\nSET 456 B"  ← "Bob"被拆开！
包3: "ob\r\n"

怎么解决？RESP的聪明之处在于每个数据块都自带长度信息：
*3\r\n        ← 数组有3个元素
$3\r\nSET\r\n ← 第一个元素：长度3，内容是"SET"
$3\r\n123\r\n ← 第二个元素：长度3，内容是"123"  
$5\r\nAlice\r\n ← 第三个元素：长度5，内容是"Alice"

如果采用这种格式，解析时可以通过前置的长度信息直接预知到我们后面的内容该如何分割。


对于RESP编码函数和日志追加函数，尧帅哥的简单生活[这篇博客](https://blog.csdn.net/Nexus_Y/article/details/157592238?spm=1001.2014.3001.5502)非常有帮助，我也是参考了他的实现。

kvstore.c
在协议解析器中支持RESP格式的解析：

```c
/*
 * @brief: support both normal text format and RESP protocol
 * @param
 * msg: request message
 * length: length of request message
 * response: need to send
 * processed: how many bytes have been encoded
 * @return: length of response
 */
#if 1
int kvs_protocol(char *msg, int length, char *response, int *processed) {
    //printf("成功调用kvs_protocol, msg = %s, length = %d\n", msg, length);
    if (msg == NULL || length < 0 || response == NULL) {
        return -1;  // 参数错误
    }
    if (processed == NULL) {
        printf("processed指针为空\n");
    }
    *processed = 0;

    // 快速检查：不是RESP格式
    //printf("成功来到判断文本类型之前\n");    
    if (length < 4 || msg[0] != '*') {
        //printf("普通文本\n");
        // 直接按普通文本处理
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = kvs_split_token(msg, tokens);
        
        if (count <= 0) {
            *processed = length;
            return -1;
        }
        
        *processed = length;
        return kvs_filter_protocol(tokens, count, response);
    }
    //printf("RESP文本\n");
    // --- RESP 解析 ---
    int pos = 1;
    int argc = 0;
    
    // 解析参数数量
    while (pos < length && msg[pos] >= '0' && msg[pos] <= '9') {
        argc = argc * 10 + (msg[pos] - '0');
        pos++;
    }
    
    // 检查CRLF
    if (pos + 1 >= length || msg[pos] != '\r' || msg[pos+1] != '\n') {
        // 格式错误，按普通文本处理
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = kvs_split_token(msg, tokens);
        
        if (count <= 0) {
            *processed = length;
            return -1;
        }
        
        *processed = length;
        return kvs_filter_protocol(tokens, count, response);
    }
    pos += 2;
    
    if (argc <= 0 || argc > KVS_MAX_TOKENS) {
        return -1;
    }
    
    char *tokens[KVS_MAX_TOKENS] = {0};
    int token_count = 0;
    
    // 解析每个参数
    for (int i = 0; i < argc; i++) {
        // 检查格式
        if (pos >= length || msg[pos] != '$') {
            goto cleanup_and_return_error;
        }
        pos++;
        
        // 解析长度
        int str_len = 0;
        while (pos < length && msg[pos] >= '0' && msg[pos] <= '9') {
            str_len = str_len * 10 + (msg[pos] - '0');
            pos++;
        }
        
        // 检查CRLF
        if (pos + 1 >= length || msg[pos] != '\r' || msg[pos+1] != '\n') {
            goto cleanup_and_return_error;
        }
        pos += 2;
        
        // 检查数据完整性
        if (str_len < 0 || pos + str_len > length) {
            *processed = 0; // 需要更多数据
            goto cleanup_and_return_error;
        }
        
        // 分配内存
        tokens[token_count] = (char*)malloc(str_len + 1);
        if (!tokens[token_count]) {
            goto cleanup_and_return_error;
        }
        
        // 复制数据
        memcpy(tokens[token_count], msg + pos, str_len);
        tokens[token_count][str_len] = '\0';
        token_count++;
        
        pos += str_len;
        
        // 检查结尾CRLF
        if (pos + 1 >= length || msg[pos] != '\r' || msg[pos+1] != '\n') {
            goto cleanup_and_return_error;
        }
        pos += 2;
    }
    
    *processed = pos;
    int result = kvs_filter_protocol(tokens, token_count, response);
    
    // 清理内存
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    
    return result;
    
cleanup_and_return_error:
    // 清理已分配的内存
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    return -1;
}
#endif
```

kvs_persist.c
从AOF文件当中恢复的函数

```c
/**
 * @brief 从AOF文件中加载，重传进协议解析函数
 * @param 文件名字符串
 */
void load_aof_file(const char *filename) {
    if (!filename) return;
    
    // 尝试打开文件
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        // 文件不存在或无法打开是正常情况（首次启动）
        printf("[Persist] File %s not found or cannot open\n", filename);
        return;
    }  
    
    printf("[Persist] Loading %s ...\n", filename);

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (fsize <= 0) {
        fclose(fp);
        printf("[Persist] File is empty\n");
        return;
    }
    
    // 分配缓冲区
    char *buffer = malloc(fsize + 1);
    if (!buffer) {
        fclose(fp);
        printf("[Persist] Memory allocation failed\n");
        return;
    }
    
    // 读取整个文件
    size_t bytes_read = fread(buffer, 1, fsize, fp);
    fclose(fp);
    
    if (bytes_read != fsize) {
        free(buffer);
        printf("[Persist] File read error\n");
        return;
    }
    
    buffer[fsize] = '\0';  // 确保以null结尾

    // 设置全局 Loading 标志位，防止加载过程触发 AOF 重写
    g_is_loading = 1; 

    char dummy_response[KVS_MAX_MSG_LEN]; // 不需要真正的回复，但接口需要
    int offset = 0;
    int processed = 0;

    // 循环处理 Buffer，解决粘包问题
    while (offset < fsize) {
        // 直接调用网络层的协议解析函数！
        int result = kvs_protocol(buffer + offset, fsize - offset, dummy_response, &processed);
        
        if (result < 0) {
            printf("[Persist] Protocol parsing error at offset %d\n", offset);
            break;
        }
        
        if (processed == 0) {
            printf("[Persist] No more data to process at offset %d\n", offset);
            break;
        }
        
        offset += processed;
    }

    g_is_loading = 0; // 恢复完成
    
    printf("[Persist] Loaded %d bytes from %s, processed %d bytes\n", 
           (int)fsize, filename, offset);
    
    free(buffer);
}
```
## 三、存量持久化

存量持久化，即从现有的数据结构当中保存key-value键值对，在存量持久化的实现当中，我采用了二进制数据格式，而不是RESP，这是为了避免解析所产生的开销，试想如果存储的value非常大，如果还需要透过解析函数来恢复数据，将非常严重地拖慢系统性能。

还是以红黑树为例，遍历红黑树，将已有的数据以二进制的形式保存到RDB文件当中：

```c
/**
 * @brief 遍历红黑树，将已有的数据以二进制的形式保存到RDB文件当中
 * @param inst 红黑树实例
 * @param node 节点
 * @param fp 文件指针
 * 
 */
void rbtree_traversal_save(rbtree *T, rbtree_node *node, FILE *fp) {
    if (!fp) return;  // 文件指针检查
    
	if (node != T->nil) {
		// 1. 遍历左子树
		rbtree_traversal_save(T, node->left, fp);
		
		// 2. 保存当前节点
#if ENABLE_KEY_CHAR
		if (node->key && node->value) {
            // 保存key
            size_t key_len = strlen(node->key);
            fwrite(&key_len, sizeof(size_t), 1, fp);
            fwrite(node->key, sizeof(char), key_len, fp);
            
            // 保存value
            size_t value_len = strlen((char *)node->value);
            fwrite(&value_len, sizeof(size_t), 1, fp);
            fwrite(node->value, sizeof(char), value_len, fp);
            
            // 调试输出（可选）
            printf("key:%s, value:%s\n", node->key, (char *)node->value);
        }
#else
        // 假设key是int，value是void*需要转换
        size_t key_len = sizeof(int);
        size_t value_len = sizeof(int); // 或者根据实际类型调整
        
        fwrite(&key_len, sizeof(size_t), 1, fp);
        fwrite(&node->key, sizeof(int), 1, fp);
        fwrite(&value_len, sizeof(size_t), 1, fp);
        fwrite(node->value, value_len, 1, fp);
        
        printf("key:%d, color:%d\n", node->key, node->color);
#endif
		
		// 3. 遍历右子树
		rbtree_traversal_save(T, node->right, fp);
	}
}

```

还是以红黑树为例，将RDB文件中的数据插回红黑树的函数实现：

```c
/**
 * @brief 从RDB文件加载数据到红黑树（二进制格式）
 * @param inst 红黑树实例
 * @param filename RDB文件名
 * @return 成功加载的键值对数量，失败返回-1
 */
int load_rdb_file(kvs_rbtree_t *inst, const char *filename) {
    if (!inst || !filename || filename[0] == '\0') {
        fprintf(stderr, "[RDB] Invalid parameters\n");
        return -1;
    }
    
    // 打开文件
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("[RDB] File not found: %s\n", filename);
        return -1;
    }
    
    int loaded_count = 0;
    
    printf("[RDB] Loading from %s\n", filename);
    
    while (!feof(fp)) {
        size_t key_len;
        
        // 1. 读取key长度
        if (fread(&key_len, sizeof(size_t), 1, fp) != 1) {
            if (feof(fp)) break;  // 正常文件结束
            fprintf(stderr, "[RDB] Failed to read key_len\n");
            fclose(fp);
            return -1;
        }
        
        // 安全检查
        if (key_len > 1024 * 1024) {
            fprintf(stderr, "[RDB] Key too large: %zu bytes\n", key_len);
            fclose(fp);
            return -1;
        }
        
        // 2. 读取key
        char *key = kvs_malloc(key_len + 1);
        if (!key) {
            fprintf(stderr, "[RDB] Memory allocation failed for key\n");
            fclose(fp);
            return -1;
        }
        
        if (fread(key, 1, key_len, fp) != key_len) {
            kvs_free(key);
            fprintf(stderr, "[RDB] Failed to read key\n");
            fclose(fp);
            return -1;
        }
        key[key_len] = '\0';
        
        // 3. 读取value长度
        size_t val_len;
        if (fread(&val_len, sizeof(size_t), 1, fp) != 1) {
            kvs_free(key);
            fprintf(stderr, "[RDB] Failed to read val_len\n");
            fclose(fp);
            return -1;
        }
        
        // 安全检查
        if (val_len > 10 * 1024 * 1024) {
            kvs_free(key);
            fprintf(stderr, "[RDB] Value too large: %zu bytes\n", val_len);
            fclose(fp);
            return -1;
        }
        
        // 4. 读取value
        char *value = kvs_malloc(val_len + 1);
        if (!value) {
            kvs_free(key);
            fprintf(stderr, "[RDB] Memory allocation failed for value\n");
            fclose(fp);
            return -1;
        }
        
        if (fread(value, 1, val_len, fp) != val_len) {
            kvs_free(key);
            kvs_free(value);
            fprintf(stderr, "[RDB] Failed to read value\n");
            fclose(fp);
            return -1;
        }
        value[val_len] = '\0';
        
        // 5. 插入到红黑树
        int result = kvs_rbtree_set(inst, key, value);
        
        if (result == 0) {
            loaded_count++;
            
            // 进度显示
            if (loaded_count % 1000 == 0) {
                printf("\r[RDB] Loading... %d items", loaded_count);
                fflush(stdout);
            }
        }
        
        // 清理临时内存（kvs_rbtree_set会复制内存）
        kvs_free(key);
        kvs_free(value);
    }
    
    fclose(fp);
    
    if (loaded_count > 0) {
        printf("\n[RDB] RBTree loaded %d items from %s\n", loaded_count, filename);
    } else {
        printf("[RDB] No data loaded from %s\n", filename);
    }
    
    return loaded_count;
}

```

