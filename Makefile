

skynet = /home/zgww/pe/skynet


libuv = $(shell pkg-config --cflags --libs libuv)
cpdir = ../teach/lib/mac
f = 

ifeq ($(shell uname), Darwin)

f += -undefined dynamic_lookup
skynet = /Users/zgww/ws/skynet
cpdir = ../teach/lib/mac/
libuv = -luv

endif

I = -I$(skynet)/skynet-src \
	-I$(skynet)/3rd/lua \
	-Ic-src

all : 
	gcc -shared -o snuv.so c-src/snuv.c -fPIC $(I) -lpthread $(libuv) $(f)
	cp snuv.so $(cpdir)

run : 
	skynet config.lua
