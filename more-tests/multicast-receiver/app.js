
const dgram = require("dgram");

const udpPort = 5010;
const multicastAddr = '239.1.1.235';

const socket = dgram.createSocket({ type: "udp4", reuseAddr: true });

socket.bind(udpPort);

socket.on("listening", function() {
    socket.addMembership(multicastAddr);
});

socket.on("message", function(msg, rinfo) {
    console.log('> ' + rinfo.address + ':' + rinfo.port);

    msg.toString().split('\n').forEach(function(line) {
        line = line.trim();
        if (line.length > 0) {
            console.log(line);
        }
    });
});
