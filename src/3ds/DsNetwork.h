#pragma once

#include <string>

namespace DsNetwork
{

// Starts the 3DS soc:U socket service over the console's configured network
// (the same settings the HOME menu uses). Safe to call more than once; failed
// attempts may be retried.
bool initialize();
bool isReady();
const std::string &localAddress();
void shutdown();

}
