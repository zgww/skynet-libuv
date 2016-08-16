#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <skynet.h>
#include <skynet_handle.h>
#include <skynet_server.h>
#include <lua.h>
#include <lauxlib.h>

#define SNUV_COPEN 1
#define SNUV_CREAD 2
#define SNUV_CWRITE 3
#define SNUV_CCLOSE 4

#define SNUV_CSPAWN 5


#define BUF_SIZE 65535

struct entry {
	//----------for skynet
	int32_t handle;
	int session;
	//----------end
	//----fs
	int read_fd;
	uv_fs_t req;
	bool is_buf_inited;
	uv_buf_t buf;


	//-----process
	uv_process_t process_req;
	uv_pipe_t pipe;
	uv_process_options_t opt;
	int exit_status;
	int term_signal;


	int cmd;
	char **argv;
	struct entry *next;
};
// send by skynet_context_send(ctx, msg, sz)
// freed after dispatch_message in skynet : free(msg)
// so entry is not freed.
// so must in lua call snuv_free_res_msg_entry
//
// result : 0 succ. < 0 : err,  >0 other meaning
struct res_msg{
	int result;
	int cmd;
	int exit_status;
	int term_signal;
	char str[0];
};

static pthread_t uv_tid;
static struct entry *entry_head = NULL;
static uv_loop_t *uv_loop;
static uv_mutex_t entry_mutex;
static uv_async_t entry_async;


static int __get_argc(char **argv){
	int i = 0;
	while (true) {
		char *arg = argv[i];
		i++;
		if (!arg) {
			break;
		}
	}
	return i;
}
static char ** __dup_argv(char **argv){
	int argc = __get_argc(argv);
	char **out_argv = malloc(sizeof(void *) * argc);
	int i;
	for (i = 0; i < argc; i++) {
		out_argv[i] = argv[i] ? strdup(argv[i]) : NULL;
	}

	return out_argv;
}
static void __free_entry(struct entry *entry){
	int i;
	for (i = 0; entry->argv[i]; i++) {
		free(entry->argv[i]);
	}
	free(entry->argv);

	if (entry->is_buf_inited)
		free(entry->buf.base);

	uv_fs_req_cleanup(&entry->req);

	// free opt.stdio
	if (entry->cmd == SNUV_CSPAWN) {
		free(entry->opt.stdio);
	}

	free(entry);
}

static void __add_entry(int32_t handle, int session, int cmd, char **argv){
	printf("new entry\n");

	struct entry *entry = calloc(1, sizeof(struct entry));
	printf("set cmd\n");
	entry->cmd = cmd;
	printf("set argv\n");
	entry->argv = __dup_argv(argv);
	printf("set handle\n");
	entry->handle = handle;
	printf("set session\n");
	entry->session = session;

	printf("set req.data = entry\n");
	// -> self
	entry->req.data = entry;
	printf("set process_req.data = entry\n");
	entry->process_req.data = entry;
	printf("set pipe.data = entry\n");
	entry->pipe.data = entry;

	printf("try lock\n");
	while (uv_mutex_trylock(&entry_mutex))
		;

	printf("get lock succ\n");
	entry->next = entry_head;
	entry_head = entry;

	printf("unlock\n");
	uv_mutex_unlock(&entry_mutex);
	printf("async send\n");
	uv_async_send(&entry_async);
}
static void __init_entry_buf(struct entry *entry, int size){
	if (entry->is_buf_inited)
	   	return;

	size = size <= 0 ? BUF_SIZE : size;
	// BUF_SIZE + 1. 1 is '\0'.
	entry->buf = uv_buf_init((char *)malloc(size + 1), size);
	entry->buf.base[size] = '\0';

	entry->is_buf_inited = true;
}
static struct entry * __detach_entry_chain(){
	while (uv_mutex_trylock(&entry_mutex))
		;
	struct entry *e = entry_head;
	entry_head = NULL;
	uv_mutex_unlock(&entry_mutex);
	return e;
}

static int __parse_flags(const char *flags){
	int flag = 0;
	bool r = strchr(flags, 'r');
	bool w = strchr(flags, 'w');
	bool a = strchr(flags, 'a');
	bool add = strchr(flags, '+');

	if (!add) {
		if (r) 
			flag = O_RDONLY;
		if (w)
			flag = O_WRONLY | O_CREAT | O_TRUNC;
		if (a)
			flag = O_WRONLY | O_CREAT | O_APPEND;
	} else {
		if (r)
			flag = O_RDWR ;
		if (w) 
			flag = O_RDWR | O_CREAT | O_TRUNC;
		if (a) 
			flag = O_RDWR | O_CREAT | O_APPEND;
	}
	printf("flag %d, r : %d, w : %d, a : %d, add : %d %d %d %d\n", flag, r, w, a, add, O_RDWR, O_RDWR | O_CREAT | O_TRUNC, O_RDWR | O_CREAT | O_TRUNC);
	return flag;
}

// notify msg will be freed by skynet
static void __notify_ccall(int result, struct entry *entry, uv_buf_t *buf){
	char *str = buf ? buf->base : entry->is_buf_inited ? entry->buf.base : NULL;
	int str_len = str ? strlen(str) : 0;

	struct res_msg *msg = calloc(1, sizeof(struct res_msg) + str_len + 1);

	msg->result = result;
	msg->cmd = entry->cmd;
	msg->exit_status = entry->exit_status;
	msg->term_signal = entry->term_signal;

	if (str) {
		strcpy(msg->str, str);
	}

	struct skynet_context *ctx = skynet_handle_grab(entry->handle);

	printf("_thread send to ctx %p handle : %d, session %d\n", ctx, entry->handle, entry->session);
	skynet_context_send(ctx, msg, sizeof(struct res_msg), 0, PTYPE_RESPONSE, entry->session);
	printf("send done\n");
}
static void __on_fs(uv_fs_t *req){
	printf("__on_fs\n");
	struct entry *entry = req->data;
	printf("__notify_call fs cmd : %d\n", entry->cmd);
	__notify_ccall(req->result, entry, NULL);

	__free_entry(entry);
}
/*
static void __on_read(uv_fs_t *req){
	struct entry *entry = req->data;

	// done or err
	if (req->result <= 0) {
		__notify_ccall(req->result, entry, NULL);
		uv_close((uv_handle_t *)req, NULL);
		return;
	}

	__notify_ccall(req->result, entry, NULL);
	
	// read next
	uv_fs_read(uv_loop, &entry->req, entry->read_fd, &entry->buf, 1, -1, __on_read);
	//__free_entry(entry);
	// reading
	//
	// error or done
	//if (req->result <= 0) {
	//}
}
*/
static void __open(struct entry *entry){
	const char *path = entry->argv[0];
	const char *flags = entry->argv[1];
	int flag = __parse_flags(flags);

	uv_fs_open(uv_loop, &entry->req, path, flag, 0, __on_fs);
}
static void __read(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);

	__init_entry_buf(entry, 0);

	entry->read_fd = fd;
	uv_fs_read(uv_loop, &entry->req, fd, &entry->buf, 1, -1, __on_fs);
}
static void __write(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);
	const char *str = entry->argv[1];
	int len = strlen(str);

	__init_entry_buf(entry, len);
	strcpy(entry->buf.base, str);
	entry->buf.len = len;

	uv_fs_write(uv_loop, &entry->req, fd, &entry->buf, 1, -1, __on_fs);
}
static void __close(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);

	uv_fs_close(uv_loop, &entry->req, fd, __on_fs);
}
static void __on_spawn_exit(uv_process_t *req, int64_t exit_status, int term_signal){
	struct entry *entry = req->data;
	entry->exit_status = exit_status;
	entry->term_signal = term_signal;

	__notify_ccall(0, entry, NULL);

	//printf("process exited with status %d %d\n", exit_status, term_signal);
	uv_close((uv_handle_t *)req, NULL);
}
static void __on_uv_read_stream(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
	uv_pipe_t *pipe = (uv_pipe_t *)stream;
	// err or done
	if (nread <= 0) {
		free(buf->base);
		uv_close((uv_handle_t *)pipe, NULL);
		return;
	}

	struct entry *entry = pipe->data;
	//buf len = suggest_len + 1
	//nread < entry.buf.len
	buf->base[nread] = '\0';

	// 1 mean : reading output
	__notify_ccall(1, entry, (uv_buf_t *)buf);
}
static void __alloc_buf(uv_handle_t *handle, size_t len, uv_buf_t *buf){
	printf("alloc buffer called. requesting %d byte \n", len);
	buf->base = malloc(len + 1);
	buf->len = len;
}

//TODO how read output of child process
static void __spawn(struct entry *entry){
	char **argv = entry->argv;

	uv_pipe_init(uv_loop, &entry->pipe, 0);


	uv_stdio_container_t *child_stdio = calloc(
			3,
		   	sizeof(uv_stdio_container_t));

	child_stdio[0].flags = UV_IGNORE; // in
	child_stdio[0].data.fd = 0;
	child_stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE; // out
	child_stdio[1].data.stream = (uv_stream_t *)&entry->pipe;
	child_stdio[2].flags = UV_IGNORE; // error
	child_stdio[2].data.fd = 2;

	entry->opt.stdio_count = 3;
	entry->opt.stdio = child_stdio;
	entry->opt.exit_cb = __on_spawn_exit;
	entry->opt.file = argv[0];
	entry->opt.args = (char **)argv;

	if (uv_spawn(uv_loop, &entry->process_req, &entry->opt)) {
		__notify_ccall(-1, entry, NULL);
	}

	uv_read_start((uv_stream_t *)&entry->pipe, __alloc_buf, __on_uv_read_stream);
}

static void __do_cmd(struct entry *entry){
	switch (entry->cmd) {
		case SNUV_COPEN : 
			__open(entry);
			break;
		case SNUV_CREAD : 
			__read(entry);
			break;
		case SNUV_CWRITE : 
			__write(entry);
			break;
		case SNUV_CCLOSE : 
			__close(entry);
			break;
		case SNUV_CSPAWN : 
			__spawn(entry);
			break;
	}
}

static void __async_cb(uv_async_t *handle){
	printf("__async cb\n");

	struct entry *entry = __detach_entry_chain();
	while (entry) {
		struct entry *next = entry->next;
		__do_cmd(entry);
		entry = next;
	}
}

static void * __uv_thread(void *ud){
	printf("______start uv\n");

	printf("__get uv loop\n");
	uv_loop = uv_default_loop();
	printf("__uv mutex init\n");
	uv_mutex_init(&entry_mutex);
	printf("__uv async init\n");
	uv_async_init(uv_loop, &entry_async, __async_cb);

	printf("__uv_run\n");
	uv_run(uv_loop, UV_RUN_DEFAULT);
}

static void __init(){
	static bool inited = false;
	if (inited) return;

	printf("snuv init\n");
	inited = true;
	printf("get uv loop %p\n", uv_loop);

	pthread_create(&uv_tid, NULL, __uv_thread, NULL);

	// 等到uv_run之后
	while (!uv_loop || !uv_loop_alive(uv_loop)) 
		sleep(0);
}

void snuv_open(int32_t handle, int session, char *path, char *flags){
	__init();

	char *argv[] = { path, flags, NULL, };
	__add_entry(handle, session, SNUV_COPEN, argv);
}
void snuv_read_str(int32_t handle, int session, int fd){
	__init();

	char fs_str[128];
	sprintf(fs_str, "%d", fd);

	char *argv[] = { fs_str, NULL, };

	__add_entry(handle, session, SNUV_CREAD, argv);
}
void snuv_write_str(int32_t handle, int session, int fd, char *str){
	__init();

	char fd_str[128];
	sprintf(fd_str, "%d", fd);

	char *argv[3];
	argv[0] = fd_str;
	argv[1] = (char *)str;
	argv[2] = NULL;

	__add_entry(handle, session, SNUV_CWRITE, argv);
}
void snuv_close(int32_t handle, int session, int fd){
	__init();

	char fd_str[128];
	sprintf(fd_str, "%d", fd);

	char *argv[] = { fd_str, NULL, };
	__add_entry(handle, session, SNUV_CCLOSE, argv);
}

// argv = {executable_file_path, ..., NULL}
// argv is not used after snuv_spawn.
// snuv_spawn use argv copy
void snuv_spawn(int32_t handle, int session, char **argv){
	__init();

	__add_entry(handle, session, SNUV_CSPAWN, argv);
}

static int lopen(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -4);
	int session = lua_tointeger(ls, -3);
	const char *path = lua_tostring(ls, -2);
	const char *mode = lua_tostring(ls, -1);

	snuv_open(handle, session, (char *)path, (char *)mode);
	return 0;
}
static int lread_str(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	int fd = lua_tointeger(ls, -1);

	snuv_read_str(handle, session, fd);
	return 0;
}
static int lwrite_str(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -4);
	int session = lua_tointeger(ls, -3);
	int fd = lua_tointeger(ls, -2);
	const char *str = lua_tostring(ls, -1);

	snuv_write_str(handle, session, fd, (char *)str);
	return 0;
}
static int lclose(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	int fd = lua_tointeger(ls, -1);

	snuv_close(handle, session, fd);
	return 0;
}
static int lspawn(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	// argv table
	// {'ls', 'arg', 'arg'}
	//
	// table at stack top
	int len = luaL_len(ls, -1); // get table len. like #table
	char **argv = calloc(len + 1, sizeof(void *));
	int i;
	for (i = 1; i <= len; i++) {
		lua_pushnumber(ls, i); // stack : i, tbl
		// get -2[-1] = tbl[i]
		// now stack : // val, tbl
		lua_gettable(ls, -2); 
		const char *arg = lua_tostring(ls, -1);
		argv[i - 1] = (char *)arg;

		lua_pop(ls, 1); // stack : tbl
	}

	snuv_spawn(handle, session, argv);
	// argv[n] is lua string. managed by lua
	free(argv);
	return 0;
}
static int lget_result(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	lua_pushinteger(ls, res_msg->result);
	return 1;
}
static int lget_cmd(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	lua_pushinteger(ls, res_msg->cmd);
	return 1;
}
static int lget_exit_status(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	lua_pushinteger(ls, res_msg->exit_status);
	return 1;
}
static int lget_term_signal(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	lua_pushinteger(ls, res_msg->term_signal);
	return 1;
}
static int lget_str(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	lua_pushstring(ls, res_msg->str);
	return 1;
}

int luaopen_snuv(lua_State *l) {
	luaL_Reg lib[] = {
		{ "open", lopen },
		{ "read_str", lread_str },
		{ "write_str", lwrite_str },
		{ "close", lclose },
		{ "spawn", lspawn },
		{ "get_result", lget_result },
		{ "get_cmd", lget_cmd },
		{ "get_exit_status", lget_exit_status },
		{ "get_term_signal", lget_term_signal },
		{ "get_str", lget_str },
		{ NULL,  NULL },
	};

	luaL_newlib(l, lib);
	return 1;
}
