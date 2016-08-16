
# ufs.lua

```
local f = ufs.open('path', 'mode')
```

暂时只读取字符串内容
```
local ctn = ufs.read_str_all(f)
local bytes = ufs.read_str(f)
ufs.write_str(f, str)

ufs.close(f)

# ufs.c


uv_async_t async;
async.data = cmd_list;


#define UFS_CMD_OPEN
#define UFS_CMD_READ
#define UFS_CMD_WRITE
#define UFS_CMD_CLOSE


#define UFS_CMD_SPAWN

cmd_list = {
	int cmd;
	// "", "str", NULL
	// end with NULL
	char **argv;
	cmd_list_entry *next;
	
}

