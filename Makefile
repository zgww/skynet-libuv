
skynet = /home/zgww/pe/skynet
f = -shared -fPIC

ifeq ($(shell uname), Darwin)

f += -undefined dynamic_lookup
skynet = /Users/zgww/ws/skynet

endif


I = -I$(skynet)/skynet-src \
	-I$(skynet)/3rd/lua \
	-Ic-src

libuv = -I/usr/local/include -L/usr/local/lib -luv


all : 
	gcc -o snuv.so c-src/snuv.c $(I) -lpthread $(libuv) $(f)
	cp snuv.so ../lua_spider/lib/mac/
	#skynet config.lua
