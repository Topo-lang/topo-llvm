#include "pipeline_api.h"
#include <topo/pipeline.h>

namespace pipeline {
TOPO_PIPELINE(int, process, (int data))
TOPO_PIPELINE(int, analyze, (int data))
TOPO_PIPELINE(int, transform, (int data))
TOPO_PIPELINE(int, compress, (int data))
}
