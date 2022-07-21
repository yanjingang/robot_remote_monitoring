# robot_remote_monitor_control
4G/5G远程视频监控和移动控制

**使用教程：
https://blog.yanjingang.com/?p=6598


**代码结构：
CameraTank：ESP32-CAM单片机程序，可以连接底盘或舵机进行控制
WebControlApp：JS写的视频显示和控制界面，通过4G网址或内嵌到微信小程序里使用
VideoStreamServer：视频流服务端，在云端服务器运行
