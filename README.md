# skynet-libuv
libuv(fs, process) on skynet.

我理解的skynet库，并未提供非阻塞的文件和system调用，
所以利用libuv来补完。

## 原理

skynet 通过 handle 来唯一标识一个服务，如果服务内部有跨服务调用，
则会用session来唯一标识这次调用，如skynet.ret, skynet.call, skynet.response。

```lua
  local session = skynet.genid()
```

libuv需要有它独立的线程，无法混入skynet调度中，所以通过以下方案来交互：

lua方，
1. 生成跨服务调用的session
2. 获取当前服务的handle
3. 调用lua-clib接口，调用C函数,该C函数会保存handle,session
4. 等C方发送响应消息
```lua
require('profile').yield('CALL', session)
```

C方：
1. 获取LUA传递过来的handle,session,并记录下来
2. 通过uv_async_t唤醒uv_loop
3. 从uv_async_t.data获取此次操作的一切信息，调用具体的uv操作,如 uv_fs_open(...)
4. 在uv_fs_cb中，将结果封装成一个SKYNET_PRESPONSE消息，发送到skynet context mq中.

