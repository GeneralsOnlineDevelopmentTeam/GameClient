#pragma once

#include "../NGMP_include.h"

class NetRoom_ChatMessagePacket : public NetworkPacket
{
public:
	NetRoom_ChatMessagePacket(AsciiString& strMessage, bool bIsAnnouncement, bool bShowAnnounceToHost);

	NetRoom_ChatMessagePacket(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;

	const std::string& GetMsg() const { return m_strMessage; }
	bool IsAnnouncement() const { return m_bIsAnnouncement; }
	bool ShowAnnouncementToHost() const { return m_bShowAnnounceToHost; }

private:
	std::string m_strMessage = "";
	bool m_bIsAnnouncement = false;
	bool m_bShowAnnounceToHost = false;
};