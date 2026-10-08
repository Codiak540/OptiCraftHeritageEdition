#include "ServerNBTStorage.h"

#include "NBTTagCompound.h"

ServerNBTStorage::ServerNBTStorage(const jstring &serverName, const jstring &serverHost) :
	name(serverName), host(serverHost), playerCount(), motd(), lag(0), polled(false)
#if defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	, nextPollTime(0), pollRetryCount(0)
#endif
{
}

NBTTagCompound *ServerNBTStorage::getCompoundTag() const
{
	NBTTagCompound *tag = new NBTTagCompound();
	tag->setString("name", name);
	tag->setString("ip", host);
	return tag;
}

ServerNBTStorage *ServerNBTStorage::createServerNBTStorage(NBTTagCompound *tag)
{
	if (tag == nullptr)
		return nullptr;
	return new ServerNBTStorage(tag->getString("name"), tag->getString("ip"));
}
