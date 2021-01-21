
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

    str = msg.toString();
    if (str.length > 0 && str.substr(str.length - 1) == '\n') {
        str = str.substr(0, str.length - 1);
    }

    console.log(str);
});
