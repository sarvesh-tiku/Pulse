#pragma once

#include "pulse/core/args.hpp"
#include "pulse/core/instrument.hpp"
#include "pulse/engine/pipeline.hpp"

namespace pulse::cli {

// Builds a PipelineConfig from shared command-line options. `session_start`
// anchors --disconnect windows (given in ms of venue time from the start).
engine::PipelineConfig make_config(const Args& a, TimestampNs session_start);
void print_run_help();

int cmd_run(const Args& a, bool replay);
int cmd_inspect(const Args& a);
int cmd_listen(const Args& a);

}  // namespace pulse::cli
