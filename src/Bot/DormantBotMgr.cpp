#include "DormantBotMgr.h"
#include "PlayerbotAI.h"
#include "MapMgr.h"
#include "CellImpl.h"
#include "GridNotifiers.h"

DormantBotMgr* DormantBotMgr::instance()
{
    static DormantBotMgr instance;
    return &instance;
}

// 白名单判定逻辑
bool DormantBotMgr::IsInWhitelist(Player* bot)
{
    if (!bot) return true;

    // 1. 随身工具人与队伍白名单：处于跟随或在真人队伍中，永久免除冬眠
    if (bot->GetGroup() && !bot->GetGroup()->IsLeaderBot())
        return true;
    
    PlayerbotAI* botAI = bot->GetPlayerbotAI();
    if (botAI && botAI->HasStrategy("follow", BotState::BOT_STATE_COMBAT))
        return true;

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

// 九宫格判定：检测是否有真实人类玩家在 5x5 单元格范围内，正常 3x3 单元格范围即可，但是为了防止高移速坐骑速度过快造成穿帮，用 5x5 范围

bool DormantBotMgr::IsPlayerNearby(uint32 mapId, float x, float y)
{
    Map* map = sMapMgr->FindBaseMap(mapId);
    if (!map) return false;

    // 魔兽底层一个 Cell 大小约为 66.6 码，检测半径大约2.5x66.6 = 166.5码
    CellCoord cellCoord = AzerothCore::ComputeCellCoord(x, y);
    
    // 遍历当前地图上所有的真实人类玩家
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* p = itr->GetSource();
        if (p && !p->GetSession()->IsBot()) // 必须是真实人类
        {
            CellCoord pCoord = AzerothCore::ComputeCellCoord(p->GetPositionX(), p->GetPositionY());
            // 计算切比雪夫距离，锁定 5x5 九宫格
            if (std::abs(int(cellCoord.x_coord) - int(pCoord.x_coord)) <= 2 &&
                std::abs(int(cellCoord.y_coord) - int(pCoord.y_coord)) <= 2)
            {
                return true; 
            }
        }
    }
    return false;
}

// 执行物理休眠：SetBotDormant(true) 核心抽象
void DormantBotMgr::PutToSleep(Player* bot)
{
    if (!bot || !bot->IsInWorld() || IsInWhitelist(bot)) return;

    Map* currentMap = bot->GetMap();
    if (!currentMap) return;

    // 注销前彻底清除运动惯性历史数据，防止机器人苏醒时出现平移（Freeze）
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MoveIdle();

    // 封存这一时刻的灵魂数据
    DormantBotInfo info;
    info.botPlayer = bot;
    info.mapId = currentMap->GetId();
    bot->GetPosition(info.posX, info.posY, info.posZ, info.posO);

    ObjectGuid guid = bot->GetGUID();
    DormantBotHolder[guid] = info; // 停尸房寄存 

    // 强行从当前的 Map 网格容器中抽离出来，但不销毁 C++ 实体指针
    currentMap->RemovePlayerFromMap(bot, false);
    
    // 改变网络状态，假装其已断开连接
    bot->SetNetState(SESSION_STATE_LOGGED_OUT); 
}

// 执行物理唤醒
void DormantBotMgr::WakeUp(ObjectGuid guid, DormantBotInfo& info)
{
    Player* bot = info.botPlayer;
    if (!bot) return;

    Map* targetMap = sMapMgr->FindBaseMap(info.mapId);
    if (!targetMap) return;

    // 网格就绪检查（Grid Ready Check）
    if (!targetMap->IsGridLoaded(info.posX, info.posY))
        return; // 地形未 100% 进入缓冲区，这一帧拒绝唤醒等待下一帧时序 

    // 唤醒时锁死原始坐标，拒绝系统动态修正高度
    bot->SetMap(targetMap);
    bot->SetPosition(info.posX, info.posY, info.posZ, info.posO);
    bot->SetNetState(SESSION_STATE_IN_GAME);

    // 物理复活：重新推入 AC 核心网格刷新体系
    targetMap->AddPlayerToMap(bot, true); 
    
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
            if (bot && bot->IsInWorld() && !IsInWhitelist(bot))
            {
                if (!IsPlayerNearby(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()))
                    toSleep.push_back(bot);
            }
        }
        for (Player* bot : toSleep) PutToSleep(bot);
        

        // 2. 扫描停尸房里的 Bot 是否需要被重新唤醒
        auto itr = DormantBotHolder.begin();
        while (itr != DormantBotHolder.end())
        {
            auto current = itr++;
            if (IsPlayerNearby(current->second.mapId, current->second.posX, current->second.posY))
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