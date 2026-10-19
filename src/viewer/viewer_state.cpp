#include "viewer/viewer_state.h"

namespace fillscope {

void Run::append(const Frame& frame) {
    frames_.push_back(frame);
    prices_.push_back(static_cast<float>(frame.price));
}

bool ViewerState::apply(const Message& message) {
    // std::visit returns what the chosen lambda returns, so both must return the same type (bool).
    return std::visit(Overloaded{
                          [&](const SourceHello& hello) {
                              run.emplace(hello);  // emplace destroys the old Run and builds the new one in place
                              return true;
                          },
                          [&](const Frame& frame) {
                              const bool started = !run.has_value();
                              if (started) {
                                  run.emplace(std::nullopt);  // frames with no description: a run that can't be replayed
                              }
                              run->append(frame);
                              return started;
                          },
                      },
                      message);
}

}  // namespace fillscope
