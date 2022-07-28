<?php
/**
 * tcp/websocket 长链接server
 * 
 * 启动：
 *  php VideoStreamServer.php
 * 停止：
 *  ps aux | grep swoole_ |grep -v grep| cut -c 9-15 | xargs kill -9
 * 
 * 测试：
 * 1.模拟硬件设备tcp长连接通信：
 *  telnet 127.0.0.1 8879
 *      {"action":"test","hid":1,"uid":"mars","data":{}}      //此命令在通讯的同时会记录hid与fd的关系
 *      quit
 * 2.模拟业务端发送http短连接指令，由server转发指令给对应硬件设备
 *  curl "http://127.0.0.1:8878?hid=1&command=abc"          //这里指定向特定hid的fd发送command，执行时tcp连接设备会收到消息
 * 
 * 3.WebGL渲染设备实时状态
 *   http://127.0.0.1:8080/swoole/websocket.php             //WebGL通过WebSocket连接127.0.0.1:8878，用于接收server push的设备动态信息     //初次连接后注册要接收状态的hid，之后持续接收设备动态用于GL渲染
 * 
 */
class SwooleServer
{
    //server ip
    public $HOST = '0.0.0.0';
    //限制最大连接数量
    public $MAX_CONNECT_NUM = 10;

    //长链接对象
    public $server = null;

    //tcp长链接服务对象，用于与硬件设备保持长链接双向通信
    public $tcpServer = null;
    //tcp长链接服务端口
    public $tcpPort = 8879;
    //当前保持tcp长链接的句柄fd列表
    public $tcpFds = null;

    //ws长链接服务端口，用于与WebGL渲染终端保持websock长链接双向通信，push渲染信息
    //*注：ws端口同时同时支持http短链接，可以通过http接收非长链接类指令
    public $wsPort = 8878;
    //当前保持websocket长链接的句柄fd列表
    public $wsFds = null;

    public function __construct()
    {
        //tcp长链接句柄fd列表初始化
        $this->tcpFds  = new swoole_table($this->MAX_CONNECT_NUM);  //key为hid设备id
        $this->tcpFds->column('fd', swoole_table::TYPE_INT);    //连接句柄
        $this->tcpFds->column('hid', swoole_table::TYPE_STRING, 32);   //tcp连接的设备id
        $this->tcpFds->column('uid', swoole_table::TYPE_STRING, 32);   //设备用户ID
        $this->tcpFds->column('ip', swoole_table::TYPE_STRING, 64);    //tcp客户端ip
        $this->tcpFds->create();

        //ws长链接句柄fd列表初始化
        $this->wsFds  = new swoole_table($this->MAX_CONNECT_NUM);   //key为ws_fd连接句柄
        $this->wsFds->column('fd', swoole_table::TYPE_INT);
        $this->wsFds->column('uid', swoole_table::TYPE_STRING, 32);   //用户ID
        $this->wsFds->column('ip', swoole_table::TYPE_STRING, 64);    //ws客户端ip
        $this->wsFds->create();

        //启动websocket/http主服务器,服务websocket WebGL渲染端和非长链接请求
        $this->server = new \swoole_websocket_server($this->HOST, $this->wsPort);
        $this->server->set([
            'daemonize' => true,    //后台运行(systemd时请勿设置)
            'work_num' => 10,       //开启worker进程数量
            'max_request' => 20,    //每个worker进程的最大任务数. 一个worker进程在处理完超过此数值的任务后将自动退出，进程退出后会释放所有内存和资源并被重新拉起。
            'task_worker_num' => 10, //开启异步task进程数量
            'log_level' => SWOOLE_LOG_TRACE,
            'log_file' => 'swoole.log',
        ]);
        $this->server->on("start", [$this, 'onStart']);
        $this->server->on('open', [$this, 'onWsOpen']);
        $this->server->on('task', [$this, 'onTask']);
        $this->server->on('finish', [$this, 'onTaskFinish']);
        $this->server->on('message', [$this, 'onWsMessage']);
        $this->server->on('close', [$this, 'onWsClose']);
        $this->server->on('request', [$this, 'onHttpRequest']);

        //添加tcp监听端口,服务硬件设备长链接请求（通讯统一）
        $this->tcpServer = $this->server->addListener($this->HOST, $this->tcpPort, SWOOLE_SOCK_TCP);
        $this->tcpServer->set([
            'open_websocket_protocol' => false,     //关闭websocket协议处理，此端口仅用于tcp
            'open_eof_check' => true,   //打开消息EOF检测
            'package_eof' => "\n",      //设置EOF符号
            'open_eof_split' => true,   //启用根据EOF自动分包（用于解决包批量send场地，但影响性能）
        ]);
        $this->tcpServer->on('connect', [$this, 'onTcpConnect']);
        $this->tcpServer->on("receive", [$this, 'onTcpReceive']);
        $this->tcpServer->on("close", [$this, 'onTcpClose']);

        
        //启动tcp/ws/http服务
        $this->server->start();

    }


    /**
     * tcp 连接进入事件
     * @param $serv
     * @param $fd
     * @return 
     */
    public function onTcpConnect($serv, $fd){
        $this->log("onTcpConnect...");
        $fdinfo = $serv->connection_info($fd);
        $this->log('onTcpConnect fdinfo: ' . var_export($fdinfo, true));
    }

    /**
     * tcp 连接关闭
     * @param $serv
     * @param $fd
     */
    public function onTcpClose($serv, $fd){
        $this->log("onTcpClose...");
        $fdinfo = $serv->connection_info($fd);
        //删除tcp连接句柄
        $hid = '';  //fd对应的设备id
        foreach ($this->tcpFds as $k => $v) {
            if ($v['fd'] == $fd) {
                $hid = $k;
                $this->tcpFds->del($k);
            }
        }
        $this->log('onTcpClose. hid=' . $hid. ' fdinfo: '. var_export($fdinfo, true));
    }


    /**
     * 监听tcp数据接收事件
     * @param $serv
     * @param $fd
     * @param $from_id
     * @param $msg
     */
    public function onTcpReceive($serv, $fd, $from_id, $msg)
    {
        $msg = trim($msg);
        $this->log("onTcpReceive. msg: [". $msg."]");
        if (empty($msg)) {
            return;
        }
        $fdinfo = $serv->connection_info($fd); //获取连接的信息
        if ($msg == 'quit'){    //客户端主动要求断开连接
            $this->server->close($fd);  //disconnect()在4.0.3以上版本才可用
            $this->log('onTcpReceive quit: client send quit command, connect is closed.' . var_export($fdinfo, true));
            return;
        }
        //解析json消息
        $data = json_decode($msg, true);
        if ($data == false) {
            $this->log('onTcpReceive format error: ' . var_export($fdinfo, true));
            return;
        }
        $data['fd'] = $fd;

        //执行异步任务
        $this->server->task($data);
    }


    /**
     * 异步任务处理
     * @param $serv         服务
     * @param $task_id    任务ID，由swoole扩展内自动生成，用于区分不同的任务
     * @param $src_worker_id $task_id和$src_worker_id组合起来才是全局唯一的，不同的worker进程投递的任务ID可能会有相同
     * @param $data 是任务的内容
     * @return mixed
     */
    public function onTask($serv, $task_id, $src_worker_id, $data)
    {
        $tcpfd = $data['fd'];      //tcp的fd
        $hid = $data['hid'];    //设备ID
        $uid = $data['uid'];    //用户ID
        $action = $data['action'];
        $fdinfo = $serv->connection_info($tcpfd);
        $this->log("onTask. action: $action  hid: $hid  uid: $uid  data:" . var_export($data, true)."\n tcp_fdinfo:" . var_export($fdinfo, true));
        
        switch ($action) {
            case "test":
                $this->log("onTask [$action] action ...");
                //对tcp硬件设备消息的处理
                $this->tcpFds->set($hid, array('fd' => $tcpfd, 'hid' => $hid, 'uid' => $uid, 'ip' => $fdinfo['remote_ip']));
                // DO SOMEING
                //找到需要渲染pid的webGL连接的fd并通知渲染更新设备信息
                if ($this->wsFds->count()) {
                    foreach ($this->wsFds as $key => $val) {
                        if ($val['uid'] == $uid) {
                            $this->server->push($val['fd'], json_encode($data));
                            $this->log("onTask [$action] action.  push ws fd [{$val['fd']}] ...");
                        }
                    }
                }
                break;
            default:
                $this->log('onTask action not found! ');
        }

        return $data; // 告诉worker  -> onTaskFinish
    }


    /**
     * worker进程投递的任务在task_worker中完成
     * @param $serv
     * @param $task_id  任务的ID
     * @param $data     任务处理的结果
     */
    public function onTaskFinish($serv, $task_id, $data){
        $fdinfo = $serv->connection_info($fd); //获取连接的信息
        $this->log('onTaskFinish. task_id: ' . $task_id. ' data: '. var_export($data, true). ' fdinfo: '. var_export($data, true));
    }

    /**
     * 主进程启动
     * @param $serv
     */
    public function onStart($serv)
    {
        $this->log('onStart...');

        //重命名进程名称
        swoole_set_process_name("swoole_master");

        //启动定时器
        $url = 'http://www.baidu.com';
        $timeout = 10000;
        /* $timerId = swoole_timer_tick($timeout, function() use ($url, $timeout) {
            $this->log("crond url: " . $url . " timeout: " . $timeout);
            $this->log("crond url response:" . $this->curl($url, json_encode(array('hello'))));
        });*/
    }


    /**
     * ws监听开启
     * @param $serv
     * @param $request
     */
    public function onWsOpen($serv, $request){
        $this->log('onWsOpen...  fd: ' . $request->fd);
    }

    /**
     * ws监听关闭
     * @param $serv
     * @param $fd
     */
    public function onWsClose($serv, $fd){
        $this->log('onWsClose...  fd: ' . $fd);
        //清除fd的连接句柄
        foreach ($this->wsFds as $k => $v) {
            if ($v['fd'] == $fd) {
                $this->wsFds->del($k);
            }
        }
    }

    /**
     * websocket监听收到来自客户端的数据
     * @param $serv
     * @param $frame
     */
    public function onWsMessage($serv, $frame){
        $this->log('onWsMessage...  frame: '. var_export($frame, true));
        $tmp = json_decode($frame->data, true);
        //更新ws映射
        $fdinfo = $serv->connection_info($frame->fd);
        $data  = array('fd' => $frame->fd, 'uid' => $tmp['uid'], 'ip' => $fdinfo['remote_ip']);
        $this->log('add wsFds: '. var_export($data, true));
        $this->wsFds->set($frame->fd, $data);
    }


    /**
     * 收到一个完整的Http请求
     * @param $request
     * @param $response
     */
    public function onHttpRequest($request, $response){
        //$data = $request->post;
        $data = $request->get;
        $this->log('onHttpRequest...  data: ' . var_export($data, true));

        //设备id
        if (!$data || !isset($data['hid'])) {
            $this->log('onHttpRequest param [hid] invlid!');
            $response->end(json_encode(array("param [hid] invlid.")));
            return;
        }
        $hid = $data['hid'];
        $command =  $data['command'];

        //获取设备对应的tcp长连接fd
        $fd = $this->tcpFds->get($hid, 'fd');
        if (empty($fd)) {
            $this->log("onHttpRequest not found hid's tcp_fd!");
            $response->end(json_encode(array("not found hid's tcp_fd.")));
            return;
        }

        //向tcp转发来自http的指令
        $ret = $this->server->send($fd, $command . "\n");
        $this->log("onHttpRequest send. tcp_fd: $fd  msg: $command res: $ret");
        $response->end(json_encode(array("code" => $ret?1:0, "msg" => $ret?"succ":"fail")));
    }


    /**
     *写日志
     * @param $msg 
     * @return 
     */
    public function log($msg, $file='server'){
        $f = fopen($file . '.log', 'a+');
        fwrite($f, date('Y-m-d H:i:s') ."\t". $msg."\n");
        fclose($f);
    }


    /**
     * 发网络请求
     * @param $url
     * @param $data
     * @param $headers
     * @return 
     */
    public function curl($url, $data, $headers = array())
    {
        if (empty($url) || empty($data)) {
            return false;
        }

        $ch = curl_init();
        curl_setopt($ch, CURLOPT_URL, $url); //抓取指定网页
        curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, 0); // 对认证证书来源的检查
        curl_setopt($ch, CURLOPT_SSL_VERIFYHOST, 1); // 从证书中检查SSL加密算法是否存在
        curl_setopt($ch, CURLOPT_POST, 1); //post提交方式

        $headers[] = "Content-type: application/json;charset='utf-8'";
        $headers[] = "Accept: application/json";
        $headers[] = "Cache-Control: no-cache";
        $headers[] = "Pragma: no-cache";
        curl_setopt($ch, CURLOPT_HTTPHEADER, $headers);
        curl_setopt($ch, CURLOPT_POSTFIELDS, $data);
        curl_setopt($ch, CURLOPT_HEADER, 0); //设置header
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true); //要求结果为字符串且输出到屏幕上
        $res = curl_exec($ch); //运行curl
        curl_close($ch);
        return $res;
    }
}


$serv = new SwooleServer();
