#include <unordered_map>
#include <set>
using namespace std;

#include "ConnectionServerSendOP.h"
#include "ConnectionServerRecvOP.h"
#include "ClientSession.h"
#include "WrapLog.h"
#include "packet.h"

#include "../../ServerConfig/ServerConfig.pb.h"
#include "./proto/LogicServerWithConnectionServer.pb.h"

#include "ClientSessionMgr.h"
#include "LogicServerSessionMgr.h"

#include "LogicServerSession.h"

extern WrapServer::PTR                      gServer;
extern WrapLog::PTR                         gDailyLogger;
extern ServerConfig::ConnectionServerConfig connectionServerConfig;

enum class CELLNET_OP :UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE
{
    LOGICSERVER_LOGIN = 3280140517,
    LOGICSERVER_DOWNSTREAM = 1648565662,
    LOGICSERVER_KICK_PLAYER = 2806502720,
    LOGICSERVER_SET_PLAYER_SLAVE = 1999458104,
    LOGICSERVER_SET_PLAYER_PRIMARY = 2933402823,
};

LogicServerSession::LogicServerSession()
{
    mIsPrimary = false;
    mID = -1;
    mSendSerialID = 1;
}

int LogicServerSession::getID() const
{
    return mID;
}

void LogicServerSession::sendPBData(UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE cmd, const char* data, size_t len)
{
    if (getEventLoop()->isInLoopThread())
    {
        helpSendPacketInLoop(cmd, data, len);
    }
    else
    {
        sendPBData(cmd, std::make_shared<string>(data, len));
    }
}

void LogicServerSession::sendPBData(UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE cmd, std::shared_ptr<string>&& data)
{
    sendPBData(cmd, data);
}

void LogicServerSession::sendPBData(UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE cmd, std::shared_ptr<string>& data)
{
    if (getEventLoop()->isInLoopThread())
    {
        helpSendPacketInLoop(cmd, data->c_str(), data->size());
    }
    else
    {
        auto sharedThis = std::static_pointer_cast<LogicServerSession>(shared_from_this());
        getEventLoop()->pushAsyncProc([=](){
            sharedThis->helpSendPacketInLoop(cmd, data->c_str(), data->size());
        });
    }
}

void LogicServerSession::helpSendPacketInLoop(UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE cmd, const char* data, size_t len)
{
    assert(getEventLoop()->isInLoopThread());

    auto ser = std::atomic_fetch_add(&mSendSerialID, static_cast<uint16_t>(1));

    char b[8 * 1024];
    BasePacketWriter packet(b, sizeof(b), false, true);
    packet.writeUINT32(cmd);
    packet.writeUINT16(ser);
    packet.writeUINT16(static_cast<uint16_t>(len + 8));
    packet.writeBuffer(data, len);

    sendPacket(packet.getData(), packet.getPos());

    gDailyLogger->debug("send logic server {}, cmd:{}, ser:{}", mID, cmd, ser);
}

void LogicServerSession::onEnter()
{
    gDailyLogger->warn("recv logic server enter");
}

void LogicServerSession::onClose()
{
    gDailyLogger->warn("recv logic server dis connect, server id : {}.", mID);

    if (mID != -1)
    {
        if (mIsPrimary)
        {
            ClientSessionMgr::KickClientOfPrimary(mID);
            LogicServerSessionMgr::RemovePrimaryLogicServer(mID);
        }
        else
        {
            LogicServerSessionMgr::RemoveSlaveLogicServer(mID);
        }

        mIsPrimary = false;
        mID = -1;
    }
}

bool LogicServerSession::checkPassword(const std::string& password)
{
    return password == connectionServerConfig.logicserverloginpassword();
}

void LogicServerSession::sendLogicServerLoginResult(bool isSuccess, const std::string& reason)
{
    internalAgreement::LogicServerLoginReply loginResult;
    loginResult.set_issuccess(isSuccess);
    loginResult.set_id(connectionServerConfig.id());

    sendPB(966232901, loginResult);
}

void LogicServerSession::procPacket(UseCellnetPacketSingleNetSession::CELLNET_OP_TYPE op, const char* body, uint16_t bodyLen)
{
    gDailyLogger->debug("recv logic server[{},{}] packet, op:{}, bodylen:{}", mID, mIsPrimary, op, bodyLen);

    BasePacketReader rp(body, bodyLen, false);
    switch (static_cast<CELLNET_OP>(op))
    {
    case CELLNET_OP::LOGICSERVER_LOGIN:
    {
        onLogicServerLogin(rp);
    }
    break;
    case CELLNET_OP::LOGICSERVER_DOWNSTREAM:
    {
        onPacket2ClientByRuntimeID(rp);
    }
    break;
    case CELLNET_OP::LOGICSERVER_KICK_PLAYER:
    {
        onKickClientByRuntimeID(rp);
    }
    break;
    case CELLNET_OP::LOGICSERVER_SET_PLAYER_SLAVE:
    {
        onIsSetPlayerSlaveServer(rp);
    }
    break;
    case CELLNET_OP::LOGICSERVER_SET_PLAYER_PRIMARY:
    {
        onSetPlayerPrimaryServer(rp);
    }
    break;
    default:
    {
        assert(false);
    }
    break;
    }

    rp.skipAll();
}

void LogicServerSession::onLogicServerLogin(BasePacketReader& rp)
{
    internalAgreement::LogicServerLogin loginMsg;
    if (loginMsg.ParseFromArray(rp.getBuffer(), static_cast<int>(rp.getMaxPos())))
    {
        gDailyLogger->info("收到逻辑服务器登陆, ID:{}, is primary:{}", loginMsg.id(), loginMsg.isprimary());

        bool loginResult = false;
        string reason;

        if (true || checkPassword(""))
        {
            bool isSuccess = false;

            if (loginMsg.isprimary())
            {
                if (LogicServerSessionMgr::FindPrimaryLogicServer(loginMsg.id()) == nullptr)
                {
                    LogicServerSessionMgr::AddPrimaryLogicServer(loginMsg.id(), std::static_pointer_cast<LogicServerSession>(shared_from_this()));
                    isSuccess = true;
                }
            }
            else
            {
                if (LogicServerSessionMgr::FindSlaveLogicServer(loginMsg.id()) == nullptr)
                {
                    LogicServerSessionMgr::AddSlaveLogicServer(loginMsg.id(), std::static_pointer_cast<LogicServerSession>(shared_from_this()));
                    isSuccess = true;
                }
            }

            if (isSuccess)
            {
                mIsPrimary = loginMsg.isprimary();
                loginResult = true;
                mID = loginMsg.id();
            }
            else
            {
                reason = "ID对应的Logic Server已存在";
            }
        }
        else
        {
            reason = "密码错误";
        }

        gDailyLogger->info("login result :{}", loginResult);

        sendLogicServerLoginResult(loginResult, reason);
    }
}

const static bool IsPrintPacketSendedLog = true;

void LogicServerSession::onPacket2ClientByRuntimeID(BasePacketReader& rp)
{
    internalAgreement::DownstreamACK downstream;
    if (downstream.ParseFromArray(rp.getBuffer(), static_cast<int>(rp.getMaxPos())))
    {
        std::shared_ptr<std::string> smartStr;

        for (auto& v : downstream.clientid())
        {
            ConnectionClientSession::PTR client = ClientSessionMgr::FindClientByRuntimeID(v);
            if (client != nullptr)
            {
                /*  如果处于同一线程则直接发送,避免分配内存(用于跨线程通信时安全的绑定数据)    */
                if (client->getEventLoop()->isInLoopThread())
                {
                    client->sendPBBinary(downstream.msgid(), downstream.data().c_str(), downstream.data().size());
                }
                else
                {
                    if (smartStr == nullptr)
                    {
                        smartStr = std::make_shared<std::string>(downstream.data().c_str(), downstream.data().size());
                    }
                    client->sendPBBinary(downstream.msgid(), smartStr);
                }
            }
        }
    }
}

void LogicServerSession::onIsSetPlayerSlaveServer(BasePacketReader& rp)
{
    internalAgreement::LogicServerSetRoleSlave setslaveMsg;
    if (setslaveMsg.ParseFromArray(rp.getBuffer(), static_cast<int>(rp.getMaxPos())))
    {
        ConnectionClientSession::PTR p = ClientSessionMgr::FindClientByRuntimeID(setslaveMsg.roleruntimeid());
        if (p != nullptr)
        {
            p->setSlaveServerID(setslaveMsg.willset() ? mID : -1);
        }
    }
}

void LogicServerSession::onSetPlayerPrimaryServer(BasePacketReader& rp)
{
    internalAgreement::LogicServerSetRolePrimary setPrimaryMsg;
    if (setPrimaryMsg.ParseFromArray(rp.getBuffer(), static_cast<int>(rp.getMaxPos())))
    {
        ConnectionClientSession::PTR p = ClientSessionMgr::FindClientByRuntimeID(setPrimaryMsg.roleruntimeid());
        if (p != nullptr)
        {
            p->setPrimaryServerID(mID);
        }
    }
}

void LogicServerSession::onKickClientByRuntimeID(BasePacketReader& rp)
{
    internalAgreement::LogicServerKickPlayer kickMsg;
    if (kickMsg.ParseFromArray(rp.getBuffer(), static_cast<int>(rp.getMaxPos())))
    {
        gDailyLogger->info("recv kick player, runtimeid:{}", kickMsg.roleruntimeid());
        ClientSessionMgr::KickClientByRuntimeID(kickMsg.roleruntimeid());
    }
}