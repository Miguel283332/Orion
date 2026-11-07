// Agente: bucle razonar -> actuar -> observar -> reflexionar, con herramientas y memoria propias.
#pragma once
#include "orion/llm.hpp"

namespace orion {

struct AgentConfig {
    int max_steps = 12;
    size_t context_chars = 9000;
};

struct GoalResult {
    std::string cycle, goal, result;
    bool finished = false, success = false;
    double score = 0.0;
    int steps = 0;
    std::vector<std::string> lessons;
};

std::optional<json> parse_action(const std::string& raw, std::string& err);

class Agent {
public:
    Agent(fs::path home, LLM& llm, Policy& pol, Audit& audit, Memory& mem, AgentConfig cfg = {}, fs::path src_root = {});
    GoalResult run_goal(const std::string& goal, int max_steps = 0, const std::string& extra = "", bool reflect = true);
    std::string chat_turn(const std::string& text);
    std::optional<std::string> free_goal();
    std::string maintenance();
    bool stop_requested() const;
    std::string system_prompt() const;
    std::string build_user_prompt(const std::string& goal, const std::vector<json>& steps, const std::string& extra, const std::string& note);

    std::function<std::string(const std::string&)> human_input;
    std::map<std::string, Tool> tools;
    SelfGate gate;
    fs::path home, workspace;
    LLM& llm;
    Policy& policy;
    Audit& audit;
    Memory& memory;
    AgentConfig cfg;

private:
    void build_tools();
    std::string ask_human(const std::string& q);
    json reflect_on(const std::string& goal, const std::vector<json>& steps, const std::string& result, bool finished, const std::string& cycle);
    fs::path src_root_;
};

// ---- evaluación: tareas verificables automáticamente
struct EvalTask {
    std::string name, goal;
    std::function<void(const fs::path&)> setup;
    std::function<bool(const fs::path&)> check;
};
std::vector<EvalTask> eval_tasks();
json run_eval(LLM& llm, const std::vector<EvalTask>& tasks, int max_steps = 8);

}  // namespace orion
