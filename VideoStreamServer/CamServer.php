<?php
/**
 * tcp/websocket 长链接server
 * 
 * 启动：
 *  php CamServer.php
 * 停止：
 *  ps aux | grep swoole_ |grep -v grep| cut -c 9-15 | xargs kill -9
 * 
 */
class CamServer
{
    //server ip
    public $HOST = '0.0.0.0';
    //限制最大连接数量
    public $MAX_CONNECT_NUM = 10;

    //长链接对象
    public $server = null;

    //ws长链接服务端口，用于与WebGL渲染终端保持websock长链接双向通信，push渲染信息
    //*注：ws端口同时同时支持http短链接，可以通过http接收非长链接类指令
    public $wsPort = 8878;
    //当前保持websocket长链接的句柄fd列表
    public $wsFds = null;

    public function __construct()
    {
        //ws长链接句柄fd列表初始化
        $this->wsFds  = new swoole_table($this->MAX_CONNECT_NUM);   //key为ws_fd连接句柄
        $this->wsFds->column('hid', swoole_table::TYPE_STRING, 32);   //要连接的设备id
        $this->wsFds->column('ip', swoole_table::TYPE_STRING, 64);    //ws客户端ip
        $this->wsFds->create();

        //启动websocket/http主服务器,服务websocket WebGL渲染端和非长链接请求
        $this->server = new \swoole_websocket_server($this->HOST, $this->wsPort);
        $this->server->set([
            'daemonize' => true,    //后台运行(systemd时请勿设置)
            'work_num' => 8,       //开启worker进程数量
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
        //$this->server->on('request', [$this, 'onHttpRequest']);
        
        //启动ws/http服务
        $this->server->start();

    }


    /**
     * 主进程启动
     * @param $serv
     */
    public function onStart($serv){
        $this->log('onStart...');
        //重命名进程名称
        swoole_set_process_name("CamServerMaster");
    }



    /**
     * ws监听开启
     * @param $serv
     * @param $request
     */
    public function onWsOpen($serv, $request){
        $this->log('onWsOpen...  fd: ' . $request->fd);
        // set fd
        $fdinfo = $serv->connection_info($request->fd);
        $this->wsFds->set($request->fd, array('hid' => 1, 'ip' => $fdinfo['remote_ip']));
    }

    /**
     * ws监听关闭
     * @param $serv
     * @param $fd
     */
    public function onWsClose($serv, $fd){
        $this->log('onWsClose...  fd: ' . $fd);
        //清除fd的连接句柄
        if(isset($this->wsFds[$fd])){
            $this->wsFds->del($fd);
        }
    }

    /**
     * websocket监听收到来自客户端的数据
     * @param $serv
     * @param $frame
     */
    public function onWsMessage($serv, $frame){
        $this->log('onWsMessage...  fd: '. $frame->fd);
        // 转发数据给其他连接者
        $this->server->task(['action'=>'videoStream','fd' => $frame->fd, 'hid' => 1, 'data' => $frame->data]);
    }


    /**
     * 异步任务处理 $this->server->task($data);
     * @param $serv         服务
     * @param $task_id    任务ID，由swoole扩展内自动生成，用于区分不同的任务
     * @param $src_worker_id $task_id和$src_worker_id组合起来才是全局唯一的，不同的worker进程投递的任务ID可能会有相同
     * @param $data 是任务的内容
     * @return mixed
     */
    public function onTask($serv, $task_id, $src_worker_id, $data){
        $action = $data['action'];
        $fd = $data['fd'];
        $hid = $data['hid'];
        //$fdinfo = $serv->connection_info($fd);
        $this->log("onTask. task_id: $task_id action: $action  hid: $hid  fd: $fd");// fdinfo:" . var_export($fdinfo, true));
        
        switch ($action) {
            case "videoStream":
                //$this->log("onTask [$action] action ...");
                //找到需要分发的fd
                if ($this->wsFds->count()) {
                    foreach ($this->wsFds as $key => $val) {
                        if ($val['hid'] == $hid && $key != $fd) {  //监听1设备数据的非自身客户端
                            $this->server->push($key, $data['data'], WEBSOCKET_OPCODE_BINARY);
                            $this->log("onTask [$action] action.  push ws fd [{$key}] ...");
                        }
                    }
                }
                break;
            default:
                $this->log('onTask action not found! ');
        }

        return $task_id; // 告诉worker-> onTaskFinish
    }


    /**
     * worker进程投递的任务在task_worker中完成
     * @param $serv
     * @param $task_id  任务的ID
     * @param $data     任务处理的结果
     */
    public function onTaskFinish($serv, $task_id, $data){
        $this->log('onTaskFinish. task_id: ' . $task_id);
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
    public function log($msg, $file='cam'){
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


$serv = new CamServer();
