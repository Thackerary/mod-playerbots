#include "DormantBotMgr.h"
#include "Unit.h"		//Player.h的父类，需要加载
#include "Player.h"    //必需引入此头文件
#include "UnitDefines.h"  // 必须包含这个
#include "SharedDefines.h"
#include "Playerbots.h"           
#include "RandomPlayerbotMgr.h"   
#include "MapMgr.h"
#include "Map.h"                   
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Group.h"                
#include "WorldSession.h"          
#include "ObjectAccessor.h"       
#include "Log.h" 

DormantBotMgr* DormantBotMgr::instance()
{
    static DormantBotMgr instance;
    return &instance;
}

// 白名单判定逻辑
bool DormantBotMgr::IsInWhitelist(Player* bot)
{
    if (!bot) return true;
	// 1. 飞行/交通工具豁免：只要在飞、在骑坐骑或在载具上，直接豁免休眠，防止穿帮和可能产生的未知的资源消耗
    if (bot->IsFlying() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->GetTransport())
        return true;

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
bool DormantBotMgr::IsPlayerNearby(uint32 mapId, uint32 instanceId, float x, float y, float z)
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
				// 如果这个真人正在乘坐狮鹫/蝙蝠，不唤醒周围假人
				if (player->HasUnitState(UNIT_STATE_IN_FLIGHT)) {
					continue; 
				}
				//如果玩家正站在飞艇、船只或地铁上，不唤醒周围假人
				if (player->GetTransport()) {
					continue;
				}
				//屏蔽飞行坐骑，不唤醒机器人，25码高度飞行检测
				if (player->IsFlying() && !player->IsFalling()) {
					float zDiff = std::abs(z - player->GetPositionZ());
					if (zDiff > 25.0f) continue;
				}
                // 采用 2D 辐射圈直接过滤，150码高移速缓冲区避坑
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
	// 清理任何bug产生的僵尸状态：如果它是“骑着空气”的僵尸，强制解绑后再休眠
    if (bot->IsMounted() && !bot->GetTransport())
    {
        bot->Dismount();
        bot->ClearUnitState(UNIT_STATE_IN_FLIGHT);
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot)) 
            botAI->Reset(true);
    }
	
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

	// 冗余保留代码，在调用 AddPlayerToMap 之前，可以额外确认一下 bot 是否还在
	if (!bot->IsInWorld()) // 只有不在世界里才加，防止重复添加
	{
		targetMap->AddPlayerToMap(bot);// 物理复活：重新推入 AC 核心网格刷新体系
	}

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
	// 冗余保留代码，物理状态重置，防止其唤醒时因各种未知bug摔死，省去复活步骤
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);
	// 彻底停止之前的惯性，防止唤醒后在空中滑行
    bot->StopMoving();
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
                if (!IsPlayerNearby(bot->GetMapId(), bot->GetInstanceId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
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
			ObjectGuid botGuid = current->first; //获取当前对象
			// 如果休眠的假人因为被远距离拉进队伍，触发了白名单豁免，原地唤醒，一定要先做白名单判断
            if (bot && IsInWhitelist(bot))
            {
				m_pendingWakeups.erase(botGuid); //把阻尼计时也关掉
                WakeUp(botGuid, current->second);
                continue; // 成功唤醒，直接跳过下方的雷达距离检测
            }
			//添加了2秒阻尼，来延迟苏醒
            if (IsPlayerNearby(current->second.mapId, current->second.instanceId, current->second.posX, current->second.posY, current->second.posZ))
            {
				// 被扫面到后不立刻唤醒，进入延迟，以排除高速移动路过的玩家唤醒机器人
                if (m_pendingWakeups.find(botGuid) == m_pendingWakeups.end())
                {
                    // 第一次发现，挂上 2000 毫秒（2秒）的阻尼倒计时
                    m_pendingWakeups[botGuid] = 2000; 
                }	
				else if (m_pendingWakeups[botGuid] <= 1000)
				//这里和m_scanTimer和diff有关，未来m_scanTimer值更改了，要相应更改
                {	// 2秒钟过去了，真人还在雷达范围内，确认不是路过，执行物理唤醒
                    WakeUp(botGuid, current->second);
                    m_pendingWakeups.erase(botGuid);
                }
				else
                    m_pendingWakeups[botGuid] -= 1000;
            }
			else
                {
                    // 玩家离开了，清零计时器
					m_pendingWakeups.erase(botGuid);
                }
        }
    }
    else
    {
        m_scanTimer -= diff;
    }
	//10分钟扫描一次由未知原因产生错误的僵尸机器人
	m_sanityCheckTimer += diff;
    if (m_sanityCheckTimer >= 600000) // 600,000 毫秒 = 10分钟
    {
        SanityCheck();
        m_sanityCheckTimer = 0; // 重置计时器
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
void DormantBotMgr::SanityCheck()
{
    // 日志记录：方便排查内存情况
    //LOG_INFO("server.loading", "DormantBotMgr: Running SanityCheck. Current dormant bots: %zu", DormantBotHolder.size());

    for (auto it = DormantBotHolder.begin(); it != DormantBotHolder.end(); )
    {
        // 逻辑：如果指针变空，或者玩家对象已经彻底销毁
        if (!it->second.botPlayer)
        {
            it = DormantBotHolder.erase(it);
        }
        else
        {
            ++it;
        }
    }
}