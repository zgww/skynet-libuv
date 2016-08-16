local skynet = require "skynet"

skynet.start(function()
	skynet.error("Watchdog listen on", 8888)

	local addr = skynet.newservice('ssay')
	skynet.send(addr, 'lua')
	skynet.exit()
end)
