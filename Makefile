

skynet = /home/zgww/pe/skynet

I = -I$(skynet)/skynet-src \
	-I$(skynet)/3rd/lua \
	-Ic-src

libuv = $(shell pkg-config --cflags --libs libuv)

all : 
	gcc -shared -o snuv.so c-src/snuv.c -fPIC $(I) -lpthread $(libuv)
	cp snuv.so ../lua_spider/lib/

run : 
	skynet config.lua
