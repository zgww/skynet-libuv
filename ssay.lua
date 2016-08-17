local skynet = require 'skynet'
local profile = require 'profile'
local snuv = require 'snuv'

local yield = profile.yield
local resume = profile.resume

local function _c(fn, h, s, ...)
	fn(h, s, ...)
	local succ, msg, sz = yield('CALL', s)
	return msg
end



skynet.start(function ()
	skynet.dispatch('lua', function (_, _)
		print(snuv)
		print(snuv.open)

		local session = skynet.genid()
		local handle = skynet.self()
		local h, s  = handle, session

		--[[
		local msg = _c(snuv.mkdir, h, s, '_mkdir')
		print(msg, snuv.get_result(msg))

		local msg = _c(snuv.rmdir, h, s, '_mkdir')
		print(msg, snuv.get_result(msg))

		local msg = _c(snuv.unlink, h, s, 'xx')
		print(msg, snuv.get_result(msg))
		local msg = _c(snuv.rename, h, s, 'xx', 'xxxxxx')
		print(msg, snuv.get_result(msg))
		local msg = _c(snuv.stat, h, s, 'xxxxxx')
		print(msg, snuv.get_result(msg),
			snuv.get_mtime_ms(msg),
			snuv.get_atime_ms(msg),
			snuv.get_ctime_ms(msg)
		)
		]]
		local msg = _c(snuv.scandir, h, s, 'xx')
		while true do
			local result = snuv.get_result(msg)
			print('lua get result ', result)
			if result <= 0 then break end

			local str = snuv.get_str(msg)
			print('str', str)

			_, msg = yield('CALL', s)
		end
		print('scandir end')

		--[[
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
