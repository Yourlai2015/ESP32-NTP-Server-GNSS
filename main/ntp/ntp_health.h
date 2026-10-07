// 本地 NTP 服务启动自检接口。
// 通过回环地址与已分配地址验证 NTP 服务是否正常。


#pragma once

// 执行启动自检：配置禁用时立即返回，否则阻塞至测试完成。
void ntp_health_run(void);
