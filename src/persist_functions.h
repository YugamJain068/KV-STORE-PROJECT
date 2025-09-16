#ifndef PERSIST_FUNCTIONS_H
#define PERSIST_FUNCTIONS_H

#include <memory>

class RaftNode; 

void persistMetadata(std::shared_ptr<RaftNode> node);
void loadMetadata(std::shared_ptr<RaftNode> node);

#endif
