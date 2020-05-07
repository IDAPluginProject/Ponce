﻿//! \file
/*
**  Copyright (c) 2016 - Ponce
**  Authors:
**         Alberto Garcia Illera        agarciaillera@gmail.com
**         Francisco Oca                francisco.oca.gonzalez@gmail.com
**
**  This program is under the terms of the BSD License.
*/

//IDA
#include <idp.hpp>
#include <dbg.hpp>
#include <loader.hpp>
#include <kernwin.hpp>

//Ponce
#include "globals.hpp"
#include "utils.hpp"
#include "callbacks.hpp"
#include "formConfiguration.hpp"
#include "formTaintSymbolizeInput.hpp"
#include "actions.hpp"
#include "formTaintWindow.hpp"
#include "blacklist.hpp"
#include "context.hpp"
#include "solver.hpp"
#include "triton_logic.hpp"

//Triton
#include "triton/api.hpp"
#include "triton/x86Specifications.hpp"


int taint_symbolize_register(const qstring& selected, action_activation_ctx_t* action_activation_ctx) {
    auto reg_id_to_symbolize = str_to_register(selected);

    if (reg_id_to_symbolize != triton::arch::register_e::ID_REG_INVALID) {
        auto register_to_symbolize = api.getRegister(reg_id_to_symbolize);
        /*When the user symbolize something for the first time we should enable step_tracing*/
        start_tainting_or_symbolic_analysis();

        msg("[+] %s register %s\n", cmdOptions.use_tainting_engine ? "Tainting" : "Symbolizing", selected.c_str());
        
        ea_t pc;
        get_ip_val(&pc);
        char comment[256];
        qsnprintf(comment, 256, "Reg %s at address: " MEM_FORMAT, selected.c_str(), pc);

        // Before symbolizing register we should set his concrete value
        needConcreteRegisterValue_cb(api, register_to_symbolize);

        if (cmdOptions.use_tainting_engine) {
            api.taintRegister(register_to_symbolize);
        }
        else{ // Symbolize register            
            api.symbolizeRegister(register_to_symbolize, std::string(comment));
        }

        tritonize(pc);
        return 0;
    }
    return 0;
}


struct ah_taint_symbolize_register_t : public action_handler_t
{
    /*Event called when the user taint a register*/
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        // Get the address range selected, or return false if there was no selection
        qstring selected;
        if (ctx->widget_type == BWN_DISASM) {      
            uint32 flags;
            get_highlight(&selected, get_current_viewer(), &flags);
        }
#if IDA_SDK_VERSION >= 740
        else if (ctx->widget_type == BWN_CPUREGS) {
            selected = ctx->regname;            
        }
#endif

        taint_symbolize_register(selected, ctx);
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* action_update_ctx)
    {
        if (action_update_ctx->widget_type == BWN_DISASM) {
            qstring selected;
            uint32 flags;
            if (get_highlight(&selected, get_current_viewer(), &flags)) {
                if (str_to_register(selected) != triton::arch::register_e::ID_REG_INVALID) {
                    char label[50] = { 0 };
                    qsnprintf(label, sizeof(label), "%s %s register", cmdOptions.use_tainting_engine ? "Taint" : "Symbolize", qstrupr((char*)selected.c_str()));

                    bool success = update_action_label(action_IDA_taint_symbolize_register.name, label);
                    success = update_action_tooltip(action_IDA_taint_symbolize_register.name, cmdOptions.use_tainting_engine ? COMMENT_TAINT_REG : COMMENT_SYMB_REG);
                    if (is_debugger_on()) return AST_ENABLE;
                }
            }
        }
#if IDA_SDK_VERSION >= 740
        else if (action_update_ctx->widget_type == BWN_CPUREGS) {
            auto reg_name = action_update_ctx->regname;
            //uint64 reg_value;
            //get_reg_val(reg_name, &reg_value); // Leave it here just in case
            if (str_to_register(reg_name) != triton::arch::register_e::ID_REG_INVALID) {
                char label[50] = { 0 };
                qsnprintf(label, sizeof(label), "%s %s register", cmdOptions.use_tainting_engine ? "Taint" : "Symbolize", reg_name);

                bool success = update_action_label(action_IDA_taint_symbolize_register.name, label);
                if (is_debugger_on()) return AST_ENABLE;
            }
        }
#endif
        return AST_DISABLE;
    }
};
static ah_taint_symbolize_register_t ah_taint_symbolize_register;

action_desc_t action_IDA_taint_symbolize_register = ACTION_DESC_LITERAL(
    "Ponce:taint_symbolize_register", // The action name. This acts like an ID and must be unique
    "nothing", //The action text.
    &ah_taint_symbolize_register, //The action handler.
    "Ctrl+Shift+R", //Optional: the action shortcut
    "Taint the selected register", //Optional: the action tooltip (available in menus/toolbar)
    50); //Optional: the action icon (shows when in menus/toolbars)

struct ah_taint_symbolize_memory_t : public action_handler_t
{
    /*Event called when the user symbolize a memory*/
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        ea_t selection_starts = 0;
        ea_t selection_ends = 0;
        ea_t current_ea = 0;
        //We ask to the user for the memory and the size
        if (ctx->widget_type == BWN_DISASM) {
            current_ea = get_screen_ea();
        }
        else if (ctx->widget_type == BWN_DUMP) {
            current_ea = get_screen_ea();
            //We get the selection bounds from the action activation context
            auto selection_starts = ctx->cur_sel.from.at->toea();
            auto selection_ends = ctx->cur_sel.to.at->toea();
            int a = 2;
        }
       
#if IDA_SDK_VERSION >= 740
        else if (ctx->widget_type == BWN_CPUREGS) {
        auto reg_name = ctx->regname;
        uint64 reg_value;
        get_reg_val(reg_name, &reg_value);
        current_ea = reg_value;
        }
#endif

        if (!prompt_window_taint_symbolize(current_ea, &selection_starts, &selection_ends))
            return 0;

        /* When the user taints something for the first time we should enable step_tracing*/
        start_tainting_or_symbolic_analysis();

        auto selection_length = selection_ends - selection_starts;
        msg("[+] %s memory from " MEM_FORMAT " to " MEM_FORMAT ". Total: %d bytes\n", cmdOptions.use_tainting_engine ? "Tainting" : "Symbolizing",  selection_starts, selection_ends, (int)selection_length);

        // Before symbolizing the memory we should set its concrete value
        for (unsigned int i = 0; i < selection_length; i++) {
            needConcreteMemoryValue_cb(api, triton::arch::MemoryAccess(selection_starts + i, 1));
        }

        if (cmdOptions.use_tainting_engine) {
            api.taintMemory(triton::arch::MemoryAccess(selection_starts, selection_length));
        }
        else{ // Symbolizing all the selected memory
            for (unsigned int i = 0; i < selection_length; i++) {
                auto symVar = api.symbolizeMemory(triton::arch::MemoryAccess(selection_starts + i, 1));
                auto var_name = symVar->getName();
                ponce_set_cmt(selection_starts + i, var_name.c_str(), true);
            }
        }

        tritonize(current_instruction());
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* action_update_ctx_t)
    {
        char label[50] = { 0 };
        bool success;
        action_state_t action_to_take = AST_DISABLE;
        if (action_update_ctx_t->widget_type == BWN_DISASM) {
            qsnprintf(label, sizeof(label), "%s memory", cmdOptions.use_tainting_engine ? "Taint" : "Symbolize");
            action_to_take = is_debugger_on() ? AST_ENABLE : AST_DISABLE;
        }
        else if (action_update_ctx_t->widget_type == BWN_DUMP) {
            qsnprintf(label, sizeof(label), "%s memory", cmdOptions.use_tainting_engine ? "Taint" : "Symbolize");
            action_to_take = is_debugger_on() ? AST_ENABLE : AST_DISABLE;    
        }
#if IDA_SDK_VERSION >= 740
        else if (action_update_ctx_t->widget_type == BWN_CPUREGS) {
            auto reg_name = action_update_ctx_t->regname;
            uint64 reg_value;  
            get_reg_val(reg_name, &reg_value);

            if (!is_mapped(reg_value)) {
                qsnprintf(label, sizeof(label), "%s memory", cmdOptions.use_tainting_engine ? "Taint" : "Symbolize");
                action_to_take = AST_DISABLE;
            }
            
            qsnprintf(label, sizeof(label), "%s memory at %s " MEM_FORMAT, cmdOptions.use_tainting_engine ? "Taint" : "Symbolize", reg_name, reg_value);    
            action_to_take = is_debugger_on() ? AST_ENABLE : AST_DISABLE;
        }
#endif
        else {
            return AST_DISABLE;
        }

        success = update_action_label(action_IDA_taint_symbolize_memory.name, label);
        success = update_action_tooltip(action_IDA_taint_symbolize_memory.name, cmdOptions.use_tainting_engine ? COMMENT_TAINT_MEM : COMMENT_SYMB_MEM);
        return action_to_take;
        
    }
};
static ah_taint_symbolize_memory_t ah_taint_symbolize_memory;

action_desc_t action_IDA_taint_symbolize_memory = ACTION_DESC_LITERAL(
    "Ponce:taint_symbolize_memory", // The action name. This acts like an ID and must be unique
    "bla bla bla", //The action text.
    &ah_taint_symbolize_memory, //The action handler.
    "Ctrl+Shift+M", //Optional: the action shortcut
    "Symbolize the selected register", //Optional: the action tooltip (available in menus/toolbar)
    50); //Optional: the action icon (shows when in menus/toolbars)

struct ah_negate_and_inject_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* action_activation_ctx)
    {
        //This is only working from the disassembly windows
        if (action_activation_ctx->widget_type == BWN_DISASM) {
            ea_t pc = action_activation_ctx->cur_ea;
            msg("[+] Negating condition at " MEM_FORMAT "\n", pc);

            /* We get the bound we are at by iterating over the pathConstraints*/
            int bound = -1;
            for (auto& path_constraint : api.getPathConstraints()) {
                bound++;
                auto constraint_address = std::get<1>(path_constraint.getBranchConstraints()[0]);
                if (pc == constraint_address) {
                    break;
                }
            }

            assert(bound != -1); // impossible

            auto solutions = solve_formula(pc, bound);
            Input* chosen_solution;
            if (solutions.size() > 0) {
                if (solutions.size() == 1) {
                    chosen_solution = &solutions[0];
                    triton::ast::SharedAbstractNode new_constraint;
                    for (auto& [taken, srcAddr, dstAddr, constraint] : api.getPathConstraints().back().getBranchConstraints()) {
                        // Let's look for the constraint we have force to take wich is the a priori not taken one
                        if (!taken) {
                            new_constraint = constraint;
                            break;
                        }
                    }
                    // Once found we first pop the last path constraint
                    api.popPathConstraint();
                    // And replace it for the found previously
                    api.pushPathConstraint(new_constraint);
                }
                else{
                    // ToDo: what do we do if we are in a switch case and get several solutions? Just using the first one? Ask the user?
                    for (const auto& solution : solutions) { 
                        // ask the user where he wants to go in popup or even better in the contextual menu
                        // chosen_solution = &solutions[0];
                        //We need to modify the last path constrain from api.getPathConstraints()
                        for (auto& [taken, srcAddr, dstAddr, constraint] : api.getPathConstraints().back().getBranchConstraints()) {
                            if (!taken) {
                            
                            }
                        }
                    }
                }

                // We negate necesary flags to go over the other branch
                negate_flag_condition(ponce_runtime_status.last_triton_instruction);

                set_SMT_solution(*chosen_solution);
            }
        }
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if process is being debugged
        if (is_debugger_on()) {
            //If we are in runtime and it is the last instruction we test if it is symbolize
            if (ponce_runtime_status.last_triton_instruction != NULL &&
                ponce_runtime_status.last_triton_instruction->getAddress() == ctx->cur_ea &&
                ponce_runtime_status.last_triton_instruction->isBranch() &&
                ponce_runtime_status.last_triton_instruction->isSymbolized()) {
                for (const auto& pc : api.getPathConstraints()) {
                    for (auto const& [taken, srcAddr, dstAddr, pc] : pc.getBranchConstraints()) {
                        if (ctx->cur_ea == srcAddr) {
                            char label[100] = { 0 };
                            qsnprintf(label, sizeof(label), "Negate and Inject to reach " MEM_FORMAT, dstAddr);
                            update_action_label(ctx->action, label);
                            return AST_ENABLE;
                        }
                    }
                }
            }
        }
        return AST_DISABLE;
    }
};
static ah_negate_and_inject_t ah_negate_and_inject;

action_desc_t action_IDA_negate_and_inject = ACTION_DESC_LITERAL(
    "Ponce:negate_and_inject", // The action name. This acts like an ID and must be unique
    "Negate and Inject", //The action text.
    &ah_negate_and_inject, //The action handler.
    "", //Optional: the action shortcut
    "Negate the current condition and inject the solution into memory", //Optional: the action tooltip (available in menus/toolbar)
    58); //Optional: the action icon (shows when in menus/toolbars)

struct ah_negate_inject_and_restore_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* action_activation_ctx)
    {
        //This is only working from the disassembly windows
        if (action_activation_ctx->widget_type == BWN_DISASM) {
            ea_t pc = action_activation_ctx->cur_ea;
            msg("[+] Negating condition at " MEM_FORMAT "\n", pc);

            //We need to get the instruction associated with this address, we look for the addres in the map
            //We want to negate the last path contraint at the current address, so we traverse the myPathconstraints in reverse

            /*unsigned int bound = ponce_runtime_status.myPathConstraints.size() - 1;
            auto solutions = solve_formula(pc, bound);*/
            //if (solutions != NULL)
            //{
            //    //Restore the snapshot
            //    snapshot.restoreSnapshot();

            //    // We set the results obtained from solve_formula
            //    set_SMT_results(input_ptr);

            //    //delete it after setting the proper results
            //    delete input_ptr;
            //}
        }
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if process is being debugged
        if (is_debugger_on() && snapshot.exists()) {
            //If we are in runtime and it is the last instruction we test if it is symbolize
            if (ponce_runtime_status.last_triton_instruction != NULL &&
                ponce_runtime_status.last_triton_instruction->getAddress() == ctx->cur_ea &&
                ponce_runtime_status.last_triton_instruction->isBranch() &&
                ponce_runtime_status.last_triton_instruction->isSymbolized()) {
                for (const auto& pc : api.getPathConstraints()) {
                    for (auto const& [taken, srcAddr, dstAddr, pc] : pc.getBranchConstraints()) {
                        if (ctx->cur_ea == srcAddr) {
                            char label[100] = { 0 };
                            qsnprintf(label, sizeof(label), "Negate, Inject to reach " MEM_FORMAT " & Restore snapshot", dstAddr);
                            update_action_label(ctx->action, label);
                            return AST_ENABLE;
                        }
                    }
                }
            }
        }
        return AST_DISABLE;
    }
};
static ah_negate_inject_and_restore_t ah_negate_inject_and_restore;

static const action_desc_t action_IDA_negateInjectRestore = ACTION_DESC_LITERAL(
    "Ponce:negate_inject_restore", // The action name. This acts like an ID and must be unique
    "Negate, Inject & Restore snapshot", //The action text.
    &ah_negate_inject_and_restore, //The action handler.
    "Ctrl+Shift+I", //Optional: the action shortcut
    "Negates a condition, inject the solution and restore the snapshot", //Optional: the action tooltip (available in menus/toolbar)
    145); //Optional: the action icon (shows when in menus/toolbars)

struct ah_create_snapshot_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        ea_t xip;
        if (!get_ip_val(&xip)) {
            msg("Could not get the XIP value. This should never happen\n");
            return 0;
        }
        ponce_set_cmt(xip, "Snapshot taken here", false);
        set_item_color(xip, 0x00FFFF);
        ponce_comments.push_back(std::make_pair(xip, 3));

        snapshot.takeSnapshot();
        snapshot.setAddress(xip); // We will use this address later to delete the comment
        msg("Snapshot Taken\n");
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if process is being debugged and there is not previous snaphot
        if (is_debugger_on() && !snapshot.exists())
            return AST_ENABLE;
        else
            return AST_DISABLE;
    }
};
static ah_create_snapshot_t ah_create_snapshot;

static const action_desc_t action_IDA_createSnapshot = ACTION_DESC_LITERAL(
    "Ponce:create_snapshot",
    "Create Execution Snapshot",
    &ah_create_snapshot,
    "Ctrl+Shift+C",
    NULL,
    129);

struct ah_restore_snapshot_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        snapshot.restoreSnapshot();
        msg("Snapshot restored\n");
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if process is being debugged and there is an existent shapshot
        if (is_debugger_on() && snapshot.exists())
            return AST_ENABLE;
        else
            return AST_DISABLE;
    }
};
static ah_restore_snapshot_t ah_restore_snapshot;

static const action_desc_t action_IDA_restoreSnapshot = ACTION_DESC_LITERAL(
    "Ponce:restore_snapshot",
    "Restore Execution Snapshot",
    &ah_restore_snapshot,
    "Ctrl+Shift+S",
    NULL,
    128);

struct ah_delete_snapshot_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        snapshot.resetEngine();
        msg("[+] Snapshot removed\n");
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if there is an existent shapshot
        if (snapshot.exists())
            return AST_ENABLE;
        else
            return AST_DISABLE;
    }
};
static ah_delete_snapshot_t ah_delete_snapshot;

static const action_desc_t action_IDA_deleteSnapshot = ACTION_DESC_LITERAL(
    "Ponce:delete_snapshot",
    "Delete Execution Snapshot",
    &ah_delete_snapshot,
    "Ctrl+Shift+D",
    NULL,
    130);

struct ah_show_config_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        prompt_conf_window();
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        return AST_ENABLE_ALWAYS;
    }
};
static ah_show_config_t ah_show_config;

action_desc_t action_IDA_show_config = ACTION_DESC_LITERAL(
    "Ponce:show_config", // The action name. This acts like an ID and must be unique
    "Show config", //The action text.
    &ah_show_config, //The action handler.
    "Ctrl+Shift+P", //Optional: the action shortcut
    "Show the Ponce configuration", //Optional: the action tooltip (available in menus/toolbar)
    156); //Optional: the action icon (shows when in menus/toolbars)

struct ah_show_taintWindow_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        //So we don't reopen twice the same window
        auto form = find_widget("Taint Window");
        if (form != NULL) {
            //let's update it and change to it
            fill_entryList();
            activate_widget(form, true);
        }

        else
            create_taint_window();

        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        update_action_label(ctx->action, cmdOptions.use_tainting_engine ? "Show Taint expressions": "Show Symbolic expressions");
        update_action_tooltip(ctx->action, cmdOptions.use_tainting_engine ? "Show all the taint expressions" : "Show all the symbolic expressions");
        return AST_ENABLE;
    }
};
static ah_show_taintWindow_t ah_show_taintWindow;

action_desc_t action_IDA_show_taintWindow = ACTION_DESC_LITERAL(
    "Ponce:show_taintWindows", // The action name. This acts like an ID and must be unique
    "Show Taint/Symbolic items", //The action text.
    &ah_show_taintWindow, //The action handler.
    "Alt+Shift+T", //Optional: the action shortcut
    "Show all the taint or symbolic items", //Optional: the action tooltip (available in menus/toolbar)
    157); //Optional: the action icon (shows when in menus/toolbars)

struct ah_unload_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        term();
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {       
        return AST_ENABLE_ALWAYS;
    }
};
static ah_unload_t ah_unload;

action_desc_t action_IDA_unload = ACTION_DESC_LITERAL(
    "Ponce:unload", // The action name. This acts like an ID and must be unique
    "Unload plugin", //The action text.
    &ah_unload, //The action handler.
    "Ctrl+Shift+U", //Optional: the action shortcut
    "Unload the plugin", //Optional: the action tooltip (available in menus/toolbar)
    138); //Optional: the action icon (shows when in menus/toolbars)

struct ah_enable_disable_tracing_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        if (ponce_runtime_status.runtimeTrigger.getState()) {
            if (ask_for_execute_native()) {
                //Deleting previous snapshot
                snapshot.resetEngine();
                //Disabling step tracing...
                disable_step_trace();
                ponce_runtime_status.runtimeTrigger.disable();
                if (cmdOptions.showDebugInfo)
                    msg("Disabling step tracing\n");
            }
        }
        else {
            start_tainting_or_symbolic_analysis();
            tritonize(current_instruction());
            if (cmdOptions.showDebugInfo)
                msg("[+] Enabling step tracing\n");
        }
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        //Only if process is being debugged
        if (is_debugger_on()) {
            //We are using this event to change the text of the action
            if (ponce_runtime_status.runtimeTrigger.getState()) {
                update_action_label(ctx->action, "Disable ponce tracing");
                update_action_icon(ctx->action, 62);
            }
            else {
                update_action_label(ctx->action, "Enable ponce tracing");
                update_action_icon(ctx->action, 61);
            }
            return AST_ENABLE;
        }
        return AST_DISABLE;
    }
};
static ah_enable_disable_tracing_t ah_enable_disable_tracing;

//We need to define this struct before the action handler because we are using it inside the handler
action_desc_t action_IDA_enable_disable_tracing = ACTION_DESC_LITERAL(
    "Ponce:enable_disable_tracing",
    "Enable ponce tracing", //The action text.
    &ah_enable_disable_tracing, //The action handler.
    "Ctrl+Shift+E", //Optional: the action shortcut
    "Enable or Disable the ponce tracing", //Optional: the action tooltip (available in menus/toolbar)
    61); //Optional: the action icon (shows when in menus/toolbars)


struct ah_solve_formula_sub_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        //We extract the solved index from the action name
        unsigned int condition_index = atoi((ctx->action+1)); // sikp [ in action name
        if (cmdOptions.showDebugInfo)
            msg("[+] Solving condition at address " MEM_FORMAT " with bound %d\n", ctx->cur_ea, condition_index);
        auto solutions = solve_formula(ctx->cur_ea, condition_index);
        
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {    
        char label[100] = { 0 };
        if (ctx->widget_type == BWN_DISASM &&
            !(is_debugger_on() && !ponce_runtime_status.runtimeTrigger.getState())) {
            // Don't let solve formulas if user is debugging natively
            for (const auto& pc : api.getPathConstraints()) {
                for (auto const& [taken, srcAddr, dstAddr, pc] : pc.getBranchConstraints()) {
                    if (ctx->cur_ea == srcAddr) {                       
                        qsnprintf(label, sizeof(label), "Solve formula to take " MEM_FORMAT, dstAddr);
                        update_action_label(ctx->action, label);
                        return AST_ENABLE;
                    }
                }
            }
        }
        qsnprintf(label, sizeof(label), "Solve formula");
        update_action_label(ctx->action, label);
        return AST_DISABLE;
    }
};
ah_solve_formula_sub_t ah_solve_formula_sub;

action_desc_t action_IDA_solve_formula_sub = ACTION_DESC_LITERAL(
    "Ponce:solve_formula_sub", // The action name. This acts like an ID and must be unique
    "Solve formula", //The action text.
    &ah_solve_formula_sub, //The action handler.
    "", //Optional: the action shortcut
    "This solves a specific conditions and shows the result in the output window", //Optional: the action tooltip (available in menus/toolbar)
    13); //Optional: the action icon (shows when in menus/toolbars)

struct ah_ponce_banner_t : public action_handler_t
{
    virtual int idaapi activate(action_activation_ctx_t* ctx)
    {
        return 0;
    }

    virtual action_state_t idaapi update(action_update_ctx_t* ctx)
    {
        update_action_label(ctx->action, cmdOptions.use_tainting_engine ? "Ponce Plugin (Taint mode)" : "Ponce Plugin (Symbolic mode)");
        return AST_ENABLE;
    }
};
static ah_ponce_banner_t ah_ponce_banner;

//We need to define this struct before the action handler because we are using it inside the handler
action_desc_t action_IDA_ponce_banner = ACTION_DESC_LITERAL(
    "Ponce:banner",
    "Ponce Plugin", //The action text.
    &ah_ponce_banner, //The action handler.
    NULL, //Optional: the action shortcut
    "Use settings below while debugging", //Optional: the action tooltip (available in menus/toolbar)
    0); //Optional: the action icon (shows when in menus/toolbars)

/*This list defined all the actions for the plugin*/
struct action action_list[] =
{
    { &action_IDA_ponce_banner, { BWN_DISASM, BWN_CPUREGS, BWN_DUMP, __END__ }, "" },

    { &action_IDA_enable_disable_tracing, { BWN_DISASM, __END__ }, "" },

#if IDA_SDK_VERSION >= 740
    { &action_IDA_taint_symbolize_register, { BWN_DISASM, BWN_CPUREGS, __END__ }, "Symbolic or taint/"},
#else
    { &action_IDA_taint_symbolize_register, { BWN_DISASM, __END__ }, "Symbolic or taint/"},
#endif
    { &action_IDA_taint_symbolize_memory, { BWN_DISASM, BWN_CPUREGS, BWN_DUMP, __END__ }, "Symbolic or taint/" },

    { &action_IDA_negate_and_inject, { BWN_DISASM, __END__ }, "SMT Solver/" },
    { &action_IDA_negateInjectRestore, { BWN_DISASM, __END__ }, "SMT Solver/" },
    { &action_IDA_solve_formula_sub, { BWN_DISASM, __END__ }, "SMT Solver/" },

    { &action_IDA_createSnapshot, { BWN_DISASM, __END__ }, "Snapshot/"},
    { &action_IDA_restoreSnapshot, { BWN_DISASM, __END__ }, "Snapshot/" },
    { &action_IDA_deleteSnapshot, { BWN_DISASM, __END__ }, "Snapshot/" },

    { NULL, __END__, __END__ }
};
