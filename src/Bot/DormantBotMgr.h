#ifndef _DORMANT_BOT_MGR_H
#define _DORMANT_BOT_MGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Player.h"
#include <unordered_map>
#include <map>	//必须引入 map 才能使用 m_pendingWakeups
//设置机器人去冬眠，而不是上下线，频繁的读取数据库占用大量服务器资源
struct DormantBotInfo
{
    Player* botPlayer;
    uint32 mapId;
    uint32 instanceId; 
    float posX;
    float posY;
    float posZ;
    float posO;
};

class DormantBotMgr
{
public:
    static DormantBotMgr* instance();

    // 核心生命周期控制
    void Update(uint32 diff);
	// 安全检查与白名单过滤
    bool IsInWhitelist(Player* bot);
	bool IsPlayerNearby(uint32 mapId, uint32 instanceId, float x, float y, float z);
    void PutToSleep(Player* bot);
    void WakeUp(ObjectGuid guid, DormantBotInfo& info);
	void ForceWakeUp(Player* bot);
	bool IsDormant(ObjectGuid guid) const { return DormantBotHolder.find(guid) != DormantBotHolder.end(); }
	void SanityCheck();//定期回收可能由未知错误而产生的无法已经在休眠，但是已经下线的机器人
private:
    DormantBotMgr() {}
    ~DormantBotMgr() {}

    // 停尸房链表：全局保活指针容器
    std::unordered_map<ObjectGuid, DormantBotInfo> DormantBotHolder; 
	uint32 m_sanityCheckTimer = 0;//为SanityCheck()用的定时器
    uint32 m_scanTimer = 1000; // 每1秒扫描一次视距，主要为了预防高移速坐骑造成穿帮，正常2000即可，此处硬编码1秒钟，可以防止playerbots自身的conf内更改后台更新时间时产生bug
	// 记录正在被唤醒倒计时的假人 GUID 和剩余时间 (毫秒),防止玩家在高速移动产生的机器人数据在服务器的读写压力
	std::map<ObjectGuid, uint32> m_pendingWakeups;
};

#define sDormantBotMgr DormantBotMgr::instance()

#endif