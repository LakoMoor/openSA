#include "StreamingIds.h"
int main(){return sa::ClassifyStreamId(25300)==sa::StreamKind::Ipl && sa::ClassifyStreamId(20000)==sa::StreamKind::Txd ? 0 : 1;}
