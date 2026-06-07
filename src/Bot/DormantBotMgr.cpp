#include "DormantBotMgr.h"
#include "Player.h"    //必需引入此头文件
#include "Playerbots.h"           
#include "RandomPlayerbotMgr.h"   
#include "MapMgr.h"
#include "Map.h"                   
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Group.h"                
#include "WorldSession.h"          
#include "ObjectAccessor.h"        

DormantBotMgr* DormantBotMgr::instance()
{
    static DormantBotMgr instance;
    return &instance;
}

// 白名单判定逻辑
bool DormantBotMgr::IsInWhitelist(Player* bot)
{
    if (!bot) return true;

    // 1. 随身工具人与队伍白名单：处于跟随或在真人队伍中，永久免除休眠
    if (Group* group = bot->GetGroup())
    {
        for (auto const& slot : group->GetMemberSlots())
        {
            if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid))
            {
                // 如果队伍里有任何一个在线玩家不是假人，说明有真人队长或队友，直接豁免
                if (!sRandomPlayerbotMgr.GetPlayerBot(member->GetGUID()))
                    return true;
            }
        }
    }
    
    // 2. 城市话痨白名单：主城与中立枢纽豁免权
    uint32 zoneId = bot->GetZoneId();
    if (zoneId == 1519 ||  // 暴风城
        zoneId == 1637 ||  // 奥格瑞玛
        zoneId == 1537 ||  // 铁炉堡
        zoneId == 4395 ||  // 达拉然
        zoneId == 3703)    // 沙塔斯 
    {
        return true;
    }

    return false;
}

// 用机器人扫描周围有没有真人
bool DormantBotMgr::IsPlayerNearby(uint32 mapId, uint32 instanceId, float x, float y)
{
	//使用 FindMap 精准定位具体的副本或大世界地图实例，包括副本和位面
    Map* currentMap = sMapMgr->FindMap(mapId, instanceId);
    if (!currentMap) return false;
	
	// 获取当前地图上所有的 Player 链表（包括真人和假人）
    Map::PlayerList const& mapPlayers = currentMap->GetPlayers();
	
	// 高速迭代器开始轮询
    for (auto itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* player = itr->GetSource();
        if (player && player->IsInWorld())
        {
            // 只有当这个人【不是】Playerbot 时，才判定为“发现了真正的玩家”
            if (!sRandomPlayerbotMgr.GetPlayerBot(player->GetGUID()))
            {
                // 采用 2D 辐射圈直接过滤，200码高移速缓冲区避坑
                if (player->GetDistance2d(x, y) <= 150.0f)//150码检测距离已经接近产生穿帮现象的极限，安全距离为200码
                {
                    return true; // 发现真正的人类，返回真
                }
            }
        }
    }
    return false;//// 遍历全图全图，附近没有一个活人
}

// 执行物理休眠：SetBotDormant(true) 核心抽象
void DormantBotMgr::PutToSleep(Player* bot)
{
    if (!bot || !bot->IsInWorld() || IsInWhitelist(bot)) return;

    Map* currentMap = bot->GetMap();
    if (!currentMap) return;

	ObjectGuid guid = bot->GetGUID();
	if (DormantBotHolder.find(guid) != DormantBotHolder.end()) return;
	
	DormantBotInfo info;
	info.botPlayer = bot;
	info.mapId = bot->GetMapId();
	info.instanceId = bot->GetInstanceId();//记录当前实例 ID，防止副本唤醒位面断层
    info.posX = bot->GetPositionX();
    info.posY = bot->GetPositionY();
    info.posZ = bot->GetPositionZ();
    info.posO = bot->GetOrientation();
	
	DormantBotHolder[guid] = info;//进入停尸房
    // 注销前彻底清除运动惯性历史数据，防止机器人苏醒时出现平移（Freeze）
    bot->StopMoving();
	
	// 核心安全修正：严禁在此处将状态改写为 LOGGED_OUT！
    // 移除从当前地图网格（Grid）中的实体绑定即可，Player::RemoveFromWorld() 底层会自动将 m_inWorld 置为 false。
    // 这时 RandomPlayerbotMgr 就会由于 !bot->IsInWorld() 自动挂起该假人的 AI 循环，内存极度安全！
	currentMap->RemovePlayerFromMap(bot, false);
}

// 执行物理唤醒
void DormantBotMgr::WakeUp(ObjectGuid guid, DormantBotInfo& info)
{
    Player* bot = info.botPlayer;
    if (!bot) return;

    Map* targetMap = sMapMgr->FindMap(info.mapId, info.instanceId);
    if (!targetMap) return;

	//如果是由于被远程邀请拉进组触发的白名单强唤，直接跳过网格卡锁，防止因旧坐标没人导致永久死锁
	if (!IsInWhitelist(bot))
    {
        // 常规自然唤醒，依然严格执行网格就绪检查，防止假人掉虚空
        if (!targetMap->IsGridLoaded(info.posX, info.posY))
            return; 
    }
    // 唤醒时锁死原始坐标，使用 GetSession() 恢复游戏网络状态
    bot->SetMap(targetMap);
    bot->Relocate(info.posX, info.posY, info.posZ, info.posO);
    // 物理复活：重新推入 AC 核心网格刷新体系
    targetMap->AddPlayerToMap(bot); 
	
	// === 【新增：强行打通 UI 渲染与队伍同步】 ===
    
	//强行触发队伍 UI 同步
    if (Group* group = bot->GetGroup())
    {
        // 强制向全队成员发送一次“当前成员列表”数据包，你的 UI 就会瞬间看到他上线了
        group->SendUpdate(); 
    }
	// 3. 强行重置 AI 状态，激活AI
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot)) 
    {
        botAI->Reset(true); 
    }
	//前提是conf文件内AiPlayerbot.SummonWhenGroup = 1以开启组队比传送到身边，如果没设置，就需要手动召唤
    DormantBotHolder.erase(guid); // 从停尸房中移出
}

// 每帧扫描轮询心跳
void DormantBotMgr::Update(uint32 diff)
{
    if (m_scanTimer <= diff)
    {
        m_scanTimer = 1000; // 复位1秒计数器
		
		// 1. 扫描在线的 Bot 是否需要进入冬眠
        std::vector<Player*> toSleep;

       
        // 遍历 mod-playerbots 管理的在线假人列表
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
			if (!bot) continue;
			if (DormantBotHolder.find(bot->GetGUID()) != DormantBotHolder.end())
                continue;
            if (bot->IsInWorld() && !IsInWhitelist(bot))
            {
                if (!IsPlayerNearby(bot->GetMapId(), bot->GetInstanceId(), bot->GetPositionX(), bot->GetPositionY()))
                    toSleep.push_back(bot);
            }
        }
        for (Player* bot : toSleep) PutToSleep(bot);
        

        // 2. 扫描停尸房里的 Bot 是否需要被重新唤醒
        auto itr = DormantBotHolder.begin();
        while (itr != DormantBotHolder.end())
        {
            auto current = itr++;
			Player* bot = current->second.botPlayer;
			// 如果休眠的假人因为被远距离拉进队伍，触发了白名单豁免，原地唤醒！
            if (bot && IsInWhitelist(bot))
            {
                WakeUp(current->first, current->second);
                continue; // 成功唤醒，直接跳过下方的雷达距离检测
            }
            if (IsPlayerNearby(current->second.mapId, current->second.instanceId, current->second.posX, current->second.posY))
            {
                WakeUp(current->first, current->second);
            }
        }
    }
    else
    {
        m_scanTimer -= diff;
    }
}
void DormantBotMgr::ForceWakeUp(Player* bot)
{
    if (!bot) return;
    
    // 注意：这里使用的是 bot->GetGUID()
    auto it = DormantBotHolder.find(bot->GetGUID());
    
    if (it != DormantBotHolder.end())
    {
        // 调用类内部的唤醒函数
        WakeUp(it->first, it->second);
    }
}