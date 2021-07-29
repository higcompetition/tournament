// Copyright 2019 DeepMind Technologies Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <thread>
#include <mutex>
#include <exception>
#include <filesystem>
#include <unistd.h>

#include "open_spiel/spiel.h"
#include "open_spiel/higc/base64.h"
#include "open_spiel/higc/referee.h"
#include "open_spiel/higc/utils.h"

namespace open_spiel {
namespace higc {

// Special messages that the bots should submit at appropriate occasions.
const std::string kReadyMessage = "ready";
const std::string kStartMessage = "start";
const std::string kPonderMessage = "ponder";
const std::string kMatchOverMessage = "match over";
const std::string kTournamentOverMessage = "tournament over";

std::unique_ptr<BotChannel> MakeBotChannel(int bot_index,
                                           std::string executable) {
  auto popen = std::make_unique<subprocess::popen>(
      std::vector<std::string>{executable});
  return std::make_unique<BotChannel>(bot_index, std::move(popen));
}

bool getline_async(int read_fd, std::string& line_out, std::string& buf) {
  int chars_read = 0;
  bool line_read = false;
  line_out.clear();

  do {
    // Read a single character (non-blocking).
    char c;
    chars_read = read(read_fd, &c, 1);
    if (chars_read == 1) {
      if (c == '\n') {
        line_out = buf;
        buf = "";
        line_read = true;
      } else {
        buf.append(1, c);
      }
    }
  } while (chars_read != 0 && !line_read);

  return line_read;
}

// Read a response message from the bot in a separate thread.
void ReadLineFromChannelStdout(BotChannel* c) {
  SPIEL_CHECK_TRUE(c);
  // Outer loop for repeated match playing.
  while (!c->shutdown_) {

    // Wait until referee sends a message to the bot.
    while (c->wait_for_message_) {
      sleep_ms(1);
      if (c->shutdown_) return;
    }

    {
      std::lock_guard<std::mutex> lock(c->mx_read);

      std::chrono::time_point time_start = std::chrono::system_clock::now();
      while (!getline_async(c->out(), c->response_, c->buf_)
          && !(c->time_out_ = time_elapsed(time_start) > c->time_limit_)
          && !c->cancel_read_) {
        sleep_ms(1);
        if (c->shutdown_) return;
      }
    }

    c->wait_for_message_ = true;
  }
}

// Global cerr mutex.
std::mutex mx_cerr;

// Read a stderr output from the bot in a separate thread.
// Forward all bot's stderr to the referee's stderr.
// Makes sure that lines are not tangled together by using a mutex.
void ReadLineFromChannelStderr(BotChannel* c) {
  SPIEL_CHECK_TRUE(c);
  int read_bytes;
  std::array<char, 1024> buf;
  while (!c->shutdown_) {
    read_bytes = read(c->err(), &buf[0], 1024);
    if (read_bytes > 0) {
      std::lock_guard<std::mutex> lock(mx_cerr);  // Have nice stderr outputs.
      std::cerr << "Bot#" << c->bot_index_ << ": ";
      for (int i = 0; i < read_bytes; ++i) std::cerr << buf[i];
      std::cerr << std::flush;
    }
    sleep_ms(1);
  }
}

void BotChannel::StartRead(int time_limit) {
  SPIEL_CHECK_FALSE(shutdown_);
  SPIEL_CHECK_TRUE(wait_for_message_);
  time_limit_ = time_limit;
  time_out_ = false;
  cancel_read_ = false;
  wait_for_message_ = false;
}

void BotChannel::CancelReadBlocking() {
  cancel_read_ = true;
  std::lock_guard<std::mutex> lock(mx_read);  // Wait until reading is cancelled.
}

void BotChannel::ShutDown() {
  shutdown_ = true;
  cancel_read_ = true;
}

// Start all players and wait for ready messages from all them simultaneously.
std::vector<bool> Referee::StartPlayers() {
  SPIEL_CHECK_EQ(game_->NumPlayers(), num_bots());

  // Launch players and create communication channels.
  log_ << "Starting players." << std::endl;
  for (int pl = 0; pl < num_bots(); ++pl) {
    const std::string& executable = executables_[pl];
    log_ << "Bot#" << pl << ": " << executable << std::endl;
    errors_.push_back(BotErrors());
    channels_.push_back(MakeBotChannel(pl, executable));
    // Read from bot's stdout/stderr in separate threads.
    threads_stdout_.push_back(std::make_unique<std::thread>(
        ReadLineFromChannelStdout, channels_.back().get()));
    threads_stderr_.push_back(std::make_unique<std::thread>(
        ReadLineFromChannelStderr, channels_.back().get()));
  }

  // Send setup information.
  const char nl = '\n';
  for (int pl = 0; pl < num_bots(); ++pl) {
    BotChannel* chn = channels_[pl].get();
    write(chn->in(), game_name_.c_str(), game_name_.size());
    write(chn->in(), &nl, 1);
    const char pl_char = '0'+pl;
    write(chn->in(), &pl_char, 1);
    write(chn->in(), &nl, 1);
    chn->StartRead(settings_.timeout_ready);
  }

  sleep_ms(settings_.timeout_ready);  // Blocking sleep to give time to the bots.
  return CheckResponses(kReadyMessage);
}

// Start a single player and wait for a ready message.
bool Referee::StartPlayer(int pl) {
  // Launch players and create communication channels.
  log_ << "Starting player " << pl << " only." << std::endl;
  const std::string& executable = executables_[pl];
  log_ << "Bot#" << pl << ": " << executable << std::endl;
  channels_[pl] = MakeBotChannel(pl, executable);
  // Read from bot's stdout/stderr in separate threads.
  threads_stdout_[pl] = std::make_unique<std::thread>(
      ReadLineFromChannelStdout, channels_.back().get());
  threads_stderr_[pl] = std::make_unique<std::thread>(
      ReadLineFromChannelStderr, channels_.back().get());

  const char nl = '\n';
  BotChannel* chn = channels_[pl].get();
  write(chn->in(), game_name_.c_str(), game_name_.size());
  write(chn->in(), &nl, 1);
  const char pl_char = '0'+pl;
  write(chn->in(), &pl_char, 1);
  write(chn->in(), &nl, 1);
  chn->StartRead(settings_.timeout_ready);

  sleep_ms(settings_.timeout_ready);  // Blocking sleep to give time to the bot.
  return CheckResponse(kReadyMessage, pl);
}

// Shut down all the players.
void Referee::ShutDownPlayers() {
  log_ << "Shutting down players." << std::endl;
  for (std::unique_ptr<BotChannel>& chn : channels_) chn->ShutDown();
  for (std::unique_ptr<std::thread>& th : threads_stdout_) th->join();
  for (std::unique_ptr<std::thread>& th : threads_stderr_) th->join();
  channels_.clear();
  threads_stdout_.clear();
  threads_stderr_.clear();
  errors_.clear();
}

// Shut down a single player.
void Referee::ShutDownPlayer(int pl) {
  log_ << "Shutting down player " << pl << " only." << std::endl;
  channels_[pl]->ShutDown();
  threads_stdout_[pl]->join();
  threads_stderr_[pl]->join();
  channels_[pl] = nullptr;
  threads_stdout_[pl] = nullptr;
  threads_stderr_[pl] = nullptr;
  errors_[pl].Reset();
}

std::unique_ptr<State> Referee::PlayMatch() {
  SPIEL_CHECK_EQ(num_bots(), game_->NumPlayers());
  std::unique_ptr<State> state = game_->NewInitialState();

  std::vector<int> player_order(num_bots());
  std::vector<bool> is_acting(num_bots(), false);
  bool only_ponder = false;  // Whether all bots only ponder (i.e chance node)
  for (int i = 0; i < num_bots(); ++i) player_order[i] = i;

  // Check start of match message.
  for (int pl = 0; pl < num_bots(); ++pl) {
    BotChannel* chn = channels_[pl].get();
    chn->StartRead(settings_.timeout_start);
  }
  sleep_ms(settings_.timeout_start);
  CheckResponses(kStartMessage);

  while (!state->IsTerminal()) {
    log_ << "\nHistory: " << absl::StrJoin(state->History(), " ") << std::endl;

    only_ponder = state->IsChanceNode();
    // Cache whether player is acting.
    for (int pl = 0; pl < num_bots(); ++pl)
      is_acting[pl] = state->IsPlayerActing(pl);
    // Make sure no player is preferred when we communicate with it.
    std::shuffle(player_order.begin(), player_order.end(), rng_);

    // Send players' observation and possibly a set of legal actions
    // available to the players.
    for (int pl : player_order) {
      BotChannel* chn = channels_[pl].get();
      public_observation_->SetFrom(*state, pl);
      private_observation_->SetFrom(*state, pl);
      std::string public_tensor = public_observation_->Compress();
      std::string private_tensor = private_observation_->Compress();

      // Send observations.
      const char space = ' ';
      base64_encode(chn->in(),
                    reinterpret_cast<char* const>(public_tensor.data()),
                    public_tensor.size());
      write(chn->in(), &space, 1);
      base64_encode(chn->in(),
                    reinterpret_cast<char* const>(private_tensor.data()),
                    private_tensor.size());
      // Send actions.
      if (is_acting[pl]) {
        std::vector<Action> legal_actions = state->LegalActions(pl);
        for (Action a : legal_actions) {
          write(chn->in(), &space, 1);
          std::string action_str = std::to_string(a);
          write(chn->in(), action_str.c_str(), action_str.size());
        }
      }
      const char nl = '\n';
      write(chn->in(), &nl, 1);
    }

    std::chrono::time_point start = std::chrono::system_clock::now();

    // Start waiting for response within the time limits.
    for (int pl: player_order) {
      BotChannel* chn = channels_[pl].get();
      chn->StartRead(is_acting[pl] ? settings_.timeout_act
                                   : settings_.timeout_ponder);
    }

    // Wait for ponder messages.
    sleep_ms(settings_.timeout_ponder);
    for (int pl = 0; pl < num_bots(); ++pl) {
      if (is_acting[pl]) continue;
      BotChannel* chn = channels_[pl].get();
      std::string response = chn->response();
      if (response != kPonderMessage) {
        log_ << "Bot#" << pl << " ponder bad response: '" << response << "'"
             << std::endl;
        errors_[pl].ponder_error++;
        if (chn->is_time_out()) {
          log_ << "Bot#" << pl << " ponder timed out." << std::endl;
          errors_[pl].time_over++;
        }
      } else {
        log_ << "Bot#" << pl << " ponder ok." << std::endl;
      }
    }

    // Wait for response(s) from acting player(s).
    // If (all) response(s) arrive before the time limit,
    // we don't have to wait to apply the action(s).
    if (!only_ponder) {
      auto has_all_responses = [&]() {
        for (int pl = 0; pl < num_bots(); ++pl) {
          if (is_acting[pl] && !channels_[pl]->has_read()) return false;
        }
        return true;
      };

      while (time_elapsed(start) < settings_.timeout_act
          && !has_all_responses())
        sleep_ms(1);

      for (int pl = 0; pl < num_bots(); ++pl) channels_[pl]->CancelReadBlocking();
    }

    // Parse submitted actions based on the bot responses.
    std::vector<Action> bot_actions(num_bots(), kInvalidAction);
    for (int pl = 0; pl < num_bots(); ++pl) {
      if (!is_acting[pl]) continue;  // Ponders have been already processed.

      BotChannel* chn = channels_[pl].get();
      std::vector<Action> legal_actions = state->LegalActions(pl);

      if (chn->is_time_out()) {
        log_ << "Bot#" << pl << " act timed out. " << std::endl;
        errors_[pl].time_over++;
      } else if (!chn->has_read()) {
        log_ << "Bot#" << pl << " act no response. " << std::endl;
        errors_[pl].protocol_error++;
      } else {
        std::string response = chn->response();
        log_ << "Bot#" << pl << " act response: '" << response << "'"
             << std::endl;

        char* end;
        int action = strtol(response.c_str(), &end, 10);
        if (*end != '\0') {
          log_ << "Bot#" << pl << " act invalid action. " << std::endl;
          errors_[pl].protocol_error++;
        } else if (std::find(legal_actions.begin(), legal_actions.end(),
                             action) == legal_actions.end()) {
          log_ << "Bot#" << pl << " act illegal action. " << std::endl;
          errors_[pl].illegal_actions++;
        } else {
          log_ << "Bot#" << pl << " act ok. " << std::endl;
          if (errors_[pl].total_errors() > settings_.max_invalid_behaviors) {
            log_ << "Bot#" << pl << " act randomly (exceeded illegal behaviors)"
                 << std::endl;
          } else {
            bot_actions[pl] = action;
          }
        }
      }

      if (bot_actions[pl] == kInvalidAction) {  // Pick a random action.
        std::uniform_int_distribution<int> dist(0, legal_actions.size() - 1);
        int random_idx = dist(rng_);
        bot_actions[pl] = legal_actions[random_idx];
      }
    }
    log_ << "Bot actions:";
    for (Action a : bot_actions) log_ << ' ' << a;
    log_ << std::endl;

    // Apply actions.
    if (state->IsChanceNode()) {
      ActionsAndProbs actions_and_probs = state->ChanceOutcomes();
      std::uniform_real_distribution<double> dist;
      const auto&[chance_action, prob] = SampleAction(actions_and_probs,
                                                      dist(rng_));
      log_ << "Chance action: " << chance_action << " with prob " << prob
           << std::endl;
      state->ApplyAction(chance_action);
    } else if (state->IsSimultaneousNode()) {
      state->ApplyActions(bot_actions);
    } else {
      state->ApplyAction(bot_actions[state->CurrentPlayer()]);
    }
  }

  std::vector<double> returns = state->Returns();

  log_ << "\nMatch over!" << std::endl;
  log_ << "History: " << absl::StrJoin(state->History(), " ") << std::endl;

  for (int pl = 0; pl < num_bots(); ++pl) {
    write(channels_[pl]->in(), kMatchOverMessage.c_str(), kMatchOverMessage.size());
    const char space = ' ';
    write(channels_[pl]->in(), &space, 1);
    int score = returns[pl];
    std::string score_str = std::to_string(score);
    write(channels_[pl]->in(), score_str.c_str(), score_str.size());
    const char nl = '\n';
    write(channels_[pl]->in(), &nl, 1);
    channels_[pl]->StartRead(settings_.timeout_match_over);
  }

  for (int pl = 0; pl < num_bots(); ++pl) {
    log_ << "Bot#" << pl << " returns " << returns[pl] << std::endl;
    log_ << "Bot#" << pl << " protocol errors " << errors_[pl].protocol_error
         << std::endl;
    log_ << "Bot#" << pl << " illegal actions " << errors_[pl].illegal_actions
         << std::endl;
    log_ << "Bot#" << pl << " ponder errors " << errors_[pl].ponder_error
         << std::endl;
    log_ << "Bot#" << pl << " time overs " << errors_[pl].time_over
         << std::endl;
  }

  sleep_ms(settings_.timeout_match_over);
  CheckResponses(kMatchOverMessage);

  return state;
}

// Response that we do not recover from.
class UnexpectedBotResponse : std::exception {};

std::vector<bool> Referee::CheckResponses(const std::string& expected_response) {
  std::vector<bool> response_ok;
  for (int pl = 0; pl < num_bots(); ++pl) {
    response_ok.push_back(CheckResponse(expected_response, pl));
  }
  return response_ok;
}

bool Referee::CheckResponse(const std::string& expected_response, int pl) {
  BotChannel* chn = channels_[pl].get();
  chn->CancelReadBlocking();
  std::string response = chn->response();
  if (response != expected_response) {
    log_ << "Bot#" << pl << " did not respond '"
         << expected_response << "'" << std::endl;
    log_ << "Bot#" << pl << " response was: '" << response << "'"
         << std::endl;
    errors_[pl].protocol_error++;
    if (chn->is_time_out()) {
      errors_[pl].time_over++;
      log_ << "Bot#" << pl << " also timed out." << std::endl;
    }
    return false;
  } else {
    log_ << "Bot#" << pl << " " << expected_response << " ok." << std::endl;
    return true;
  }
}

void Referee::TournamentOver() {
  for (int pl = 0; pl < num_bots(); ++pl) {
    write(channels_[pl]->in(), kTournamentOverMessage.c_str(), kTournamentOverMessage.size());
    const char nl = '\n';
    write(channels_[pl]->in(), &nl, 1);
  }
  sleep_ms(settings_.time_tournament_over);
  // Do not check the final message.
}

void Referee::ResetErrorTracking() {
  for (BotErrors& e : errors_) e.Reset();
}

bool Referee::corrupted_match_due(int pl) const {
  return errors_[pl].total_errors() > settings_.max_invalid_behaviors
      || errors_[pl].protocol_error > 0;
}

void Referee::RestartPlayer(int pl) {
  ShutDownPlayer(pl);
  StartPlayer(pl);
}

Referee::Referee(const std::string& game_name,
                 const std::vector<std::string>& executables,
                 int seed,
                 TournamentSettings settings,
                 std::ostream& log)
    : game_name_(game_name),
      game_(LoadGame(game_name)),
      public_observer_(game_->MakeObserver(kPublicObsType, {})),
      private_observer_(game_->MakeObserver(kPrivateObsType, {})),
      public_observation_(
          std::make_unique<Observation>(*game_, public_observer_)),
      private_observation_(
          std::make_unique<Observation>(*game_, private_observer_)),
      executables_(executables), settings_(settings), rng_(seed), log_(log) {
  SPIEL_CHECK_FALSE(executables_.empty());

  for (const std::string& executable : executables_) {
    std::filesystem::path f(executable);
    if (!std::filesystem::exists(f)) {
      SpielFatalError(absl::StrCat(
          "The bot file '", executable, "' was not found."));
    }
    if (access(executable.c_str(), X_OK) != 0) {
      SpielFatalError(absl::StrCat(
          "The bot file '", executable, "' cannot be executed. "
                                        "(missing +x flag?)"));
    }
  }
}

std::unique_ptr<TournamentResults> Referee::PlayTournament(int num_matches) {
  auto results = std::make_unique<TournamentResults>(num_bots());
  std::vector<bool> start_ok = StartPlayers();
  bool all_ok = true;
  for (int pl = 0; pl < num_bots(); ++pl) {
    all_ok = all_ok && start_ok[pl];
    if (!start_ok[pl]) results->corrupted_matches[pl] = num_matches;
  }
  if (!all_ok) {
    log_ << "Could not start all players correctly, "
            "cannot play the tournament." << std::endl;
    return results;
  }

  const int corruption_threshold =
      num_matches * settings().disqualification_rate;
  int match;
  for (match = 0; match < num_matches; ++match) {
    log_ << "\n";
    for (int j = 0; j < 80; ++j) log_ << '-';
    log_ << "\nPlaying match " << match + 1 << " / " << num_matches
         << std::endl;
    for (int j = 0; j < 80; ++j) log_ << '-';
    log_ << std::endl;

    ResetErrorTracking();
    std::unique_ptr<State> state = PlayMatch();
    std::vector<double> returns = state->Returns();

    // Update mean,var statistics.
    results->history_len_mean +=
        (state->FullHistory().size() - results->history_len_mean) / (match + 1.);
    for (int pl = 0; pl < num_bots(); ++pl) {
      double delta = returns[pl] - results->returns_mean[pl];
      results->returns_mean[pl] += delta / (match + 1.);
      double delta2 = returns[pl] - results->returns_mean[pl];
      results->returns_agg[pl] += delta * delta2;
    }
    // Disqualifications update.
    for (int pl = 0; pl < num_bots(); ++pl) {
      if (!corrupted_match_due(pl)) continue;
      log_ << "Bot#" << pl << " exceeded illegal behaviors in match "
           << match << std::endl;
      results->corrupted_matches[pl]++;

      if (results->corrupted_matches[pl] > corruption_threshold) {
        log_ << "Bot#" << pl << " is disqualified!" << std::endl;
        results->disqualified[pl] = true;
        TournamentOver();
        return results;
      } else {
        log_ << "Bot#" << pl << " is going to restart!" << std::endl;
        ++results->restarts[pl];
        RestartPlayer(pl);
      }
    }

    results->matches.push_back(MatchResult{
      .terminal = std::move(state),
      .errors = errors_
    });
  }

  log_ << "\n";
  for (int j = 0; j < 80; ++j) log_ << '-';
  log_ << "\nTournament is over!" << std::endl;
  for (int j = 0; j < 80; ++j) log_ << '-';
  log_ << std::endl;

  results->PrintVerbose(log_);
  TournamentOver();

  return results;
}

void BotErrors::Reset() {
  protocol_error = 0;
  illegal_actions = 0;
  ponder_error = 0;
  time_over = 0;
}

int BotErrors::total_errors() const {
  return protocol_error + illegal_actions + ponder_error + time_over;
}

TournamentResults::TournamentResults(int num_bots)
    : num_bots(num_bots),
      returns_mean(num_bots, 0.),
      returns_agg(num_bots, 0.),
      corrupted_matches(num_bots, 0),
      disqualified(num_bots, false),
      restarts(num_bots, 0) {}

void TournamentResults::PrintVerbose(std::ostream& os) {
  os << "In total played " << num_matches() << " matches." << std::endl;
  os << "Average length of a match was " << history_len_mean
       << " actions." << std::endl;
  os << "\nCorruption statistics:" << std::endl;
  for (int pl = 0; pl < num_bots; ++pl) {
    os << "Bot#" << pl << ": " << corrupted_matches[pl] << '\n';
  }

  os << "\nReturns statistics:" << std::endl;
  for (int pl = 0; pl < num_bots; ++pl) {
    double mean = returns_mean[pl];
    double var = returns_agg[pl] / (num_matches());
    os << "Bot#" << pl
         << " mean: " << mean
         << " var: " << var << std::endl;
  }
}

void TournamentResults::PrintCsv(std::ostream& os, bool print_header) {
  if (print_header) {
    os << "history,";
    for (int pl = 0; pl < num_bots; ++pl) {
      os << "returns[" << pl << "],"
            "protocol_error[" << pl << "],"
            "illegal_actions[" << pl << "],"
            "ponder_error[" << pl << "],"
            "time_over[" << pl << "]";
    }
    os << std::endl;
  }
  for (const MatchResult& match : matches) {
    os << absl::StrJoin(match.terminal->History(), " ");
    for (int pl = 0; pl < num_bots; ++pl) {
      os << ','
         << match.terminal->Returns()[pl] << ','
         << match.errors[pl].protocol_error << ','
         << match.errors[pl].illegal_actions << ','
         << match.errors[pl].ponder_error << ','
         << match.errors[pl].time_over;
    }
    os << std::endl;
  }
}

}  // namespace higc
}  // namespace open_spiel
