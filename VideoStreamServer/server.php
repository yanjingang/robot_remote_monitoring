<?php
/**
 * ws camera stream server
 *	php server.php 后台守护进程运行
 */

//连接本地的 Redis 服务
$redis = new Redis();
$redis->connect('127.0.0.1', 6379);
$redis->auth('zAqxSw@cDe#');
$redis->set("fd", "[]"); //每次第一次执行都需要先清空reids里面的标识

//创建ws server
$server = new swoole_websocket_server("0.0.0.0", 8079);   #要安全组中放行
$server->set([
                'daemonize' => true,//进程守护 可以后台运行
            ]);

$server->on('open', function (swoole_websocket_server $server, $request) use($redis) {
    $str = json_decode($redis->get("fd"), true);
    if($str == "") $str = [];
    if(!in_array($request->fd, $str)){
        array_push($str, $request->fd);
        $str = json_encode($str);
        $redis->set("fd", $str);
    }
});

$server->on('message', function (swoole_websocket_server $server, $frame) use($redis) {
    $str = json_decode($redis->get("fd"), true);
    foreach ($str as $key => $value) {
        if($frame->fd != $value){
            $server->push($value, $frame->data, WEBSOCKET_OPCODE_BINARY);
        }
    }
});

$server->on('close', function ($ser, $fd) use($redis) {
    $str = json_decode($redis->get("fd"), true);
    $point = array_keys($str, $fd, true);
    array_splice($str, $point['0'],1);
    $str = json_encode($str);
    $redis->set("fd", $str);
});

$server->start();

