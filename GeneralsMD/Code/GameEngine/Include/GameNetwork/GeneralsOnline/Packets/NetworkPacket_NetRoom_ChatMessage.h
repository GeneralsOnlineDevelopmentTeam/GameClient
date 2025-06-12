#pragma once

#include "../NGMP_include.h"

class NetRoom_ChatMessagePacket : public NetworkPacket
{
public:
	NetRoom_ChatMessagePacket(AsciiString& strMessage, bool bIsAnnouncement, bool bShowAnnounceToHost, bool bIsAction);

	NetRoom_ChatMessagePacket(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;

	const std::string& GetMsg() const { return m_strMessage; }
	bool IsAnnouncement() const { return m_bIsAnnouncement; }
	bool ShowAnnouncementToHost() const { return m_bShowAnnounceToHost; }
	bool IsAction() const { return m_bIsAction; }

private:
	std::string m_strMessage = "";
	bool m_bIsAnnouncement = false;
	bool m_bShowAnnounceToHost = false;
	bool m_bIsAction = false;
};