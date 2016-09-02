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
#define SNUV_CMKDIR 5
#define SNUV_CRMDIR 6
#define SNUV_CSCANDIR 7
#define SNUV_CSTAT 8
#define SNUV_CRENAME 9
#define SNUV_CUNLINK 10

#define SNUV_CSPAWN 100


#define BUF_SIZE 65535

static int _default_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
static int _mkdir_defa_mode = S_IRWXU | S_IRWXG | S_IRWXO;

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

	bool has_stat;
	uv_stat_t stat;

	int str_len;
	char str[0];
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
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
	//printf("new entry\n");

	struct entry *entry = calloc(1, sizeof(struct entry));
	//printf("set cmd\n");
	entry->cmd = cmd;
	//printf("set argv\n");
	entry->argv = __dup_argv(argv);
	//printf("set handle\n");
	entry->handle = handle;
	//printf("set session\n");
	entry->session = session;

	//printf("set req.data = entry\n");
	// -> self
	entry->req.data = entry;
	//printf("set process_req.data = entry\n");
	entry->process_req.data = entry;
	//printf("set pipe.data = entry\n");
	entry->pipe.data = entry;

	//printf("try lock %d\n", (int)pthread_self());
	while (uv_mutex_trylock(&entry_mutex))
		;

	//printf("get lock succ\n");
	entry->next = entry_head;
	entry_head = entry;

	//printf("unlock\n");
	uv_mutex_unlock(&entry_mutex);
	//printf("unlock succ\n");
	//printf("async send\n");
	uv_async_send(&entry_async);
	//printf("async send succ\n");
}
static void __init_entry_buf(struct entry *entry, int size){
	//printf("__init_entry_buf, usr size %d\n", size);
	if (entry->is_buf_inited)
	   	return;

	size = size <= 0 ? BUF_SIZE : size;
	//printf("__init_entry_buf, actual  size %d\n", size);
	// BUF_SIZE + 1. 1 is '\0'.
	entry->buf = uv_buf_init((char *)malloc(size + 1), size);
	entry->buf.base[size] = '\0';
	//printf("init buf len : %d\n", entry->buf.len);

	entry->is_buf_inited = true;
}
static struct entry * __detach_entry_chain(){
	//printf("__detach entry chain. trylock\n");
	while (uv_mutex_trylock(&entry_mutex))
		;
	struct entry *e = entry_head;
	entry_head = NULL;
	//printf("__detach entry chain unlock\n");
	uv_mutex_unlock(&entry_mutex);
	//printf("__detach entry chain unlock succ\n");
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
	//printf("flag %d, r : %d, w : %d, a : %d, add : %d %d %d %d\n", flag, r, w, a, add, O_RDWR, O_RDWR | O_CREAT | O_TRUNC, O_RDWR | O_CREAT | O_TRUNC);
	return flag;
}
static void __fill_msg_stat(struct res_msg *msg, int result, struct entry *entry){
	if (entry->cmd == SNUV_CSTAT 
			&& result >= 0){
		msg->has_stat = true;
		msg->stat = entry->req.statbuf;
	}
}

// notify msg will be freed by skynet
static void __notify_ccall(int result, struct entry *entry, char *str, int str_len){
	struct res_msg *msg = calloc(1, sizeof(struct res_msg) + str_len + 1);

	msg->result = result;
	msg->cmd = entry->cmd;
	msg->exit_status = entry->exit_status;
	msg->term_signal = entry->term_signal;

	if (str) {
		strcpy(msg->str, str);
		msg->str_len = str_len;
	}
	__fill_msg_stat(msg, result, entry);

	struct skynet_context *ctx = skynet_handle_grab(entry->handle);

	//printf("to %p:%d:%d. r : %d, cmd : %d, exit_status : %d, term_signal : %d, str : %s\n", 
	//		ctx, entry->handle, entry->session, 
	//		result, msg->cmd, msg->exit_status, msg->term_signal, str ? str : "");

	skynet_context_send(ctx, msg, sizeof(struct res_msg), 0, PTYPE_RESPONSE, entry->session);
	//printf("send done\n");
}
static void __on_fs(uv_fs_t *req){
	//printf("__on_fs\n");
	struct entry *entry = req->data;
	//printf("__notify_call fs cmd : %d\n", entry->cmd);
	__notify_ccall(req->result, entry, NULL, 0);

	__free_entry(entry);
}
static void __on_scandir(uv_fs_t *req){
	//printf("__on_scandir result %d\n", (int)req->result);
	struct entry *entry = req->data;

	uv_dirent_t dent;
	if (req->result < 0) {
		__notify_ccall(req->result, entry, NULL, 0);
		goto __FREE__;
	}

	while (UV_EOF != uv_fs_scandir_next(req, &dent)){
		//printf("scan : %s, is dir : %s\n", dent.name, dent.type == UV_DIRENT_DIR ? "yes" : "no");
		__notify_ccall(dent.type == UV_DIRENT_DIR ? 1 : 2 , entry, (char *)dent.name, strlen((char *)dent.name));
	}
	__notify_ccall(0, entry, NULL, 0);

__FREE__ : 
	__free_entry(entry);
}
static void __on_read(uv_fs_t *req){
	struct entry *entry = req->data;
	//printf("__on read result : %d, max : %d:\n", req->result, entry->buf.len);

	// done or err
	if (req->result <= 0) {
		__notify_ccall(req->result, entry, NULL, 0);
		goto __FREE__;
	}
	entry->buf.base[req->result] = '\0';

	//printf("%s\n", entry->buf.base);
	__notify_ccall(req->result, entry, entry->buf.base, req->result);

__FREE__ :
	__free_entry(entry);
	
	// read next
	//uv_fs_read(uv_loop, &entry->req, entry->read_fd, &entry->buf, 1, -1, __on_read);
	//__free_entry(entry);
	// reading
	//
	// error or done
	//if (req->result <= 0) {
	//}
}
static void __open(struct entry *entry){
	const char *path = entry->argv[0];
	const char *flags = entry->argv[1];
	int flag = __parse_flags(flags);

	uv_fs_open(uv_loop, &entry->req, path, flag, _default_mode, __on_fs);
}
static void __read(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);

	//printf("__read__init entry buf\n");
	__init_entry_buf(entry, 0);

	entry->read_fd = fd;
	uv_fs_read(uv_loop, &entry->req, fd, &entry->buf, 1, -1, __on_read);
}
static void __write(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);
	const char *str = entry->argv[1];

	const char *len_str = entry->argv[2];
	size_t len = (size_t)atoi(len_str);

	__init_entry_buf(entry, len);
	strncpy(entry->buf.base, str, len);
	entry->buf.len = len;

	uv_fs_write(uv_loop, &entry->req, fd, &entry->buf, 1, -1, __on_fs);
}
static void __close(struct entry *entry){
	const char *fd_str = entry->argv[0];
	int fd = atoi(fd_str);

	uv_fs_close(uv_loop, &entry->req, fd, __on_fs);
}
static void __mkdir(struct entry *entry){
	const char *path = entry->argv[0];
	uv_fs_mkdir(uv_loop, &entry->req, path, _mkdir_defa_mode, __on_fs);
}
static void __rmdir(struct entry *entry){
	const char *path = entry->argv[0];
	uv_fs_rmdir(uv_loop, &entry->req, path, __on_fs);
}
static void __scandir(struct entry *entry){
	const char *path = entry->argv[0];
	uv_fs_scandir(uv_loop, &entry->req, path, 0, __on_scandir);
}
static void __stat(struct entry *entry){
	const char *path = entry->argv[0];
	uv_fs_stat(uv_loop, &entry->req, path, __on_fs);
}
static void __rename(struct entry *entry){
	const char *path = entry->argv[0];
	const char *new_path = entry->argv[1];

	uv_fs_rename(uv_loop, &entry->req, path, new_path, __on_fs);
}
static void __unlink(struct entry *entry){
	const char *path = entry->argv[0];
	uv_fs_unlink(uv_loop, &entry->req, path, __on_fs);
}
static void __on_spawn_exit(uv_process_t *req, int64_t exit_status, int term_signal){
	////printf("__on_spawn_exit\n");
	struct entry *entry = req->data;
	entry->exit_status = exit_status;
	entry->term_signal = term_signal;

	//printf("__on_spawn_exit result : 0, exit status : %d, term signal : %d\n", (int)exit_status, term_signal);
	__notify_ccall(0, entry, NULL, 0);

	//printf("process exited with status %d %d\n", exit_status, term_signal);
	//printf("uv_close spawn req\n");
	uv_close((uv_handle_t *)req, NULL);
	//printf("uv_close spawn req done\n");

	__free_entry(entry);
}
static void __on_uv_read_stream(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
	//printf("__on_uv_read_stream. nread : %ld\n", (long)nread);
	uv_pipe_t *pipe = (uv_pipe_t *)stream;
	// err or done
	if (nread <= 0) {
		//printf("__on_uv_read_stream. free then close pipe\n");
		free(buf->base);
		uv_close((uv_handle_t *)pipe, NULL);
		//printf("__on_uv_read_stream. free then close pipe done\n");
		return;
	}

	struct entry *entry = pipe->data;
	//buf len = suggest_len + 1
	//nread < entry.buf.len
	//buf->base[nread] = '\0'; // 其实没有意义。 应该使用buf->len
	//printf("what pipe read : \n%s$--\n", buf->base);

	// 1 mean : reading output
	__notify_ccall(1, entry, (char *)buf->base, nread);
}
static void __alloc_buf(uv_handle_t *handle, size_t len, uv_buf_t *buf){
	//printf("alloc buffer called. requesting %ld byte \n", len);
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
		__notify_ccall(-1, entry, NULL, 0);
	}

	uv_read_start((uv_stream_t *)&entry->pipe, __alloc_buf, __on_uv_read_stream);
}

static void __do_cmd(struct entry *entry){
	//printf("__do_cmd %d\n", (int)pthread_self());
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
		case SNUV_CMKDIR : 
			__mkdir(entry);
			break;
		case SNUV_CRMDIR : 
			__rmdir(entry);
			break;
		case SNUV_CSCANDIR : 
			__scandir(entry);
			break;
		case SNUV_CSTAT : 
			__stat(entry);
			break;
		case SNUV_CRENAME : 
			__rename(entry);
			break;
		case SNUV_CUNLINK : 
			__unlink(entry);
			break;
	}
}

static void __async_cb(uv_async_t *handle){
	//printf("__async cb %d\n", (int)pthread_self());

	struct entry *entry = __detach_entry_chain();
	while (entry) {
		struct entry *next = entry->next;
		__do_cmd(entry);
		entry = next;
	}
	//printf("__async cb end\n");
}

static void * __uv_thread(void *ud){
	//printf("______start uv\n");

	//printf("__get uv loop\n");
	uv_loop = uv_default_loop();
	//printf("__uv_run\n");
	uv_run(uv_loop, UV_RUN_DEFAULT);
}

// 使用pthread_once,保证在多线程环境下只调用一次
static void __init(){
	//printf("------------------------------\n");
	//printf("__init\n");
	//printf("------------------------------\n");

	//printf("__uv mutex init\n");
	uv_mutex_init(&entry_mutex);
	//printf("__uv async init\n");
	uv_async_init(uv_default_loop(), &entry_async, __async_cb);

	pthread_create(&uv_tid, NULL, __uv_thread, NULL);

	// 等到uv_run之后
	while (!uv_loop || !uv_loop_alive(uv_loop)) 
		sleep(0);
}

static void snuv_open(int32_t handle, int session, char *path, char *flags){
	pthread_once(&once, &__init);

	char *argv[] = { path, flags, NULL, };
	__add_entry(handle, session, SNUV_COPEN, argv);
}
static void snuv_read_str(int32_t handle, int session, int fd){
	pthread_once(&once, &__init);

	char fs_str[128];
	sprintf(fs_str, "%d", fd);

	char *argv[] = { fs_str, NULL, };

	__add_entry(handle, session, SNUV_CREAD, argv);
}
static void snuv_write_str(int32_t handle, int session, int fd, char *str, size_t len){
	pthread_once(&once, &__init);

	char fd_str[128];
	sprintf(fd_str, "%d", fd);
	char len_str[128];
	sprintf(len_str, "%ld", (long)len);

	char *argv[3];
	argv[0] = fd_str;
	argv[1] = (char *)str;
	argv[2] = len_str;
	argv[3] = NULL;

	__add_entry(handle, session, SNUV_CWRITE, argv);
}
static void snuv_close(int32_t handle, int session, int fd){
	pthread_once(&once, &__init);

	char fd_str[128];
	sprintf(fd_str, "%d", fd);

	char *argv[] = { fd_str, NULL, };
	__add_entry(handle, session, SNUV_CCLOSE, argv);
}

// argv = {executable_file_path, ..., NULL}
// argv is not used after snuv_spawn.
// snuv_spawn use argv copy
static void snuv_spawn(int32_t handle, int session, char **argv){
	pthread_once(&once, &__init);

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
	size_t len;
	const char *str = lua_tolstring(ls, -1, &len);

	snuv_write_str(handle, session, fd, (char *)str, len);
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
	//printf("lget_str str_len is %d\n", res_msg->str_len);
	if (res_msg->str_len > 0) {
		lua_pushlstring(ls, res_msg->str, res_msg->str_len);
	} else{
		lua_pushnil(ls);
	}
	return 1;
}
static long __cv_timespec_ms(uv_timespec_t t){
	long ns = t.tv_nsec;
	long ms = t.tv_sec * 1000;
	long n_ms = ns / 1000 / 1000;
	return ms + n_ms;
}
static int __push_st_time(lua_State *ls, struct res_msg *res_msg, uv_timespec_t t){
	if (res_msg->has_stat) {
		lua_pushinteger(ls, __cv_timespec_ms(t));
	} else {
		lua_pushnil(ls);
	}
	return 1;
}
static int lget_mtime_ms(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	return __push_st_time(ls, res_msg, res_msg->stat.st_mtim);
}
static int lget_ctime_ms(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	return __push_st_time(ls, res_msg, res_msg->stat.st_ctim);
}
static int lget_atime_ms(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	return __push_st_time(ls, res_msg, res_msg->stat.st_atim);
}
static int lis_stat_dir(lua_State *ls){
	struct res_msg *res_msg = lua_touserdata(ls, -1);
	if (res_msg->has_stat) {
		uint64_t mode = res_msg->stat.st_mode;
		lua_pushboolean(ls, S_ISDIR(mode));
		return 1;
	}
	return 0;
}

static int lmkdir(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	const char *path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CMKDIR, argv);
	return 0;
}
static int lrmdir(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	const char *path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CRMDIR, argv);
	return 0;
}
static int lscandir(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	const char *path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CSCANDIR, argv);
	return 0;
}
static int l_stat(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	const char *path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CSTAT, argv);
	return 0;
}
static int lrename(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -4);
	int session = lua_tointeger(ls, -3);
	const char *path = lua_tostring(ls, -2);
	const char *new_path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, (char *)new_path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CRENAME, argv);
	return 0;
}
static int lunlink(lua_State *ls){
	int32_t handle = (int32_t)lua_tointeger(ls, -3);
	int session = lua_tointeger(ls, -2);
	const char *path = lua_tostring(ls, -1);

	pthread_once(&once, &__init);

	char *argv[] = {
		(char *)path, NULL,
	};

	// TODO no mode support yet
	__add_entry(handle, session, SNUV_CUNLINK, argv);
	return 0;
}

int luaopen_snuv(lua_State *l) {
	luaL_Reg lib[] = {
		{ "open", lopen },
		{ "read", lread_str },
		{ "write", lwrite_str },
		{ "close", lclose },
		{ "spawn", lspawn },

		{ "mkdir", lmkdir },
		{ "rmdir", lrmdir },
		{ "scandir", lscandir },
		{ "stat", l_stat },
		{ "rename", lrename },
		{ "unlink", lunlink },

		{ "get_result", lget_result },
		{ "get_cmd", lget_cmd },
		{ "get_exit_status", lget_exit_status },
		{ "get_term_signal", lget_term_signal },
		{ "get_str", lget_str },

		{ "get_mtime_ms", lget_mtime_ms },
		{ "get_ctime_ms", lget_ctime_ms },
		{ "get_atime_ms", lget_atime_ms },
		{ "is_stat_dir", lis_stat_dir },

		{ NULL,  NULL },
	};

	luaL_newlib(l, lib);
	return 1;
}
