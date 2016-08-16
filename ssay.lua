local skynet = require 'skynet'
local profile = require 'profile'
local snuv = require 'snuv'

local yield = profile.yield
local resume = profile.resume



skynet.start(function ()
	skynet.dispatch('lua', function (_, _)
		print(snuv)
		print(snuv.open)

		local session = skynet.genid()
		local handle = skynet.self()
		snuv.spawn(handle, session, {
			'ls', '-al'
		})
		while true do
			local succ, msg, sz = yield('CALL', session)
			local fd = snuv.get_result(msg)
			print('yield retun ', succ, msg, sz)
			print('result ', snuv.get_result(msg))
			print('cmd  ', snuv.get_cmd(msg))
			print('str  ', snuv.get_str(msg))

			if snuv.get_result(msg) <= 0 then
				break
			end
		end
		print('spawn done')
		--[[

		snuv.open(handle, session, 'Makefile', 'w+')
		local succ, msg, sz = yield('CALL', session)
		local fd = snuv.get_result(msg)
		print('yield retun ', succ, msg, sz)
		print('result ', snuv.get_result(msg))
		print('cmd  ', snuv.get_cmd(msg))
		print('str  ', snuv.get_str(msg))


		snuv.read_str(handle, session, fd)
		local succ, msg, sz = yield('CALL', session)
		print('yield retun ', succ, msg, sz)
		print('result ', snuv.get_result(msg))
		print('cmd  ', snuv.get_cmd(msg))
		print('str  ', snuv.get_str(msg))

		snuv.write_str(handle, session, fd, 'sdgsdgsdigsdgls')
		local succ, msg, sz = yield('CALL', session)
		print('yield retun ', succ, msg, sz)
		print('result ', snuv.get_result(msg))
		print('cmd  ', snuv.get_cmd(msg))
		print('str  ', snuv.get_str(msg))

		snuv.close(handle, session, fd)
		local succ, msg, sz = yield('CALL', session)
		print('yield retun ', succ, msg, sz)
		print('result ', snuv.get_result(msg))
		print('cmd  ', snuv.get_cmd(msg))
		print('str  ', snuv.get_str(msg))
		]]

	end)
end)
