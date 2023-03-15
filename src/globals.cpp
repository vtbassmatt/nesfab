#include "globals.hpp"

#include <ostream>
#include <fstream>

#include "alloca.hpp"
#include "bitset.hpp"
#include "compiler_error.hpp"
#include "fnv1a.hpp"
#include "o.hpp"
#include "options.hpp"
#include "byteify.hpp"
#include "cg.hpp"
#include "graphviz.hpp"
#include "thread.hpp"
#include "guard.hpp"
#include "group.hpp"
#include "ram_alloc.hpp"
#include "eval.hpp"
#include "rom.hpp"
#include "ir_util.hpp"
#include "ir_algo.hpp"
#include "debug_print.hpp"
#include "text.hpp"
#include "switch.hpp"

global_t& global_t::lookup(char const* source, pstring_t name)
{
    auto& global = lookup_sourceless(name, name.view(source));
    return global;
}

global_t& global_t::lookup_sourceless(pstring_t name, std::string_view key)
{
    std::uint64_t const hash = fnv1a<std::uint64_t>::hash(key.data(), key.size());

    return *global_ht::with_pool([&, hash, key](auto& pool)
    {
        rh::apair<global_t**, bool> result = global_pool_map.emplace(hash,
            [key](global_t* ptr) -> bool
            {
                return std::equal(key.begin(), key.end(), ptr->name.begin(), ptr->name.end());
            },
            [&pool, name, key]() -> global_t*
            { 
                return &pool.emplace_back(name, key, pool.size());
            });

        return *result.first;
    });
}

global_t* global_t::lookup_sourceless(std::string_view view)
{
    std::uint64_t const hash = fnv1a<std::uint64_t>::hash(view.data(), view.size());

    return global_ht::with_const_pool([&, hash, view](auto const&)
    {
        auto result = global_pool_map.lookup(hash,
            [view](global_t* ptr) -> bool
            {
                return std::equal(view.begin(), view.end(), ptr->name.begin(), ptr->name.end());
            });

        return result.second ? *result.second : nullptr;
    });
}

// Changes a global from UNDEFINED to some specified 'gclass'.
// This gets called whenever a global is parsed.
unsigned global_t::define(pstring_t pstring, global_class_t gclass, 
                          ideps_map_t&& ideps, std::function<unsigned(global_t&)> create_impl)
{
    assert(compiler_phase() <= PHASE_PARSE);
    unsigned ret;
    {
        std::lock_guard<std::mutex> global_lock(m_define_mutex);
        if(m_gclass != GLOBAL_UNDEFINED)
        {
            if(pstring && m_pstring)
            {
                file_contents_t file(pstring.file_i);
                throw compiler_error_t(
                    fmt_error(pstring, fmt("Global identifier % already in use.", name), &file)
                    + fmt_note(m_pstring, "Previous definition here:"));
            }
            else
                throw compiler_error_t(fmt("Global identifier % already in use.", name));
        }

        m_gclass = gclass;
        assert(pstring);
        m_pstring = pstring; // Not necessary but useful for error reporting.
        m_impl_id = ret = create_impl(*this);
        m_ideps = std::move(ideps);
    }
    ideps.clear();
    return ret;
}

fn_ht global_t::define_fn(pstring_t pstring, ideps_map_t&& ideps,
                          type_t type, fn_def_t&& fn_def, std::unique_ptr<mods_t> mods, 
                          fn_class_t fclass, bool iasm)
{
    fn_t* ret;

    // Create the fn
    fn_ht h = { define(pstring, GLOBAL_FN, std::move(ideps), [&](global_t& g)
    { 
        return fn_ht::pool_emplace(
            ret, g, type, std::move(fn_def), std::move(mods), fclass, iasm).id; 
    }) };

    if(fclass == FN_MODE)
    {
        std::lock_guard<std::mutex> lock(modes_vec_mutex);
        modes_vec.push_back(ret);
    }
    else if(fclass == FN_NMI)
    {
        std::lock_guard<std::mutex> lock(nmi_vec_mutex);
        nmi_vec.push_back(ret);
    }
    else if(fclass == FN_IRQ)
    {
        std::lock_guard<std::mutex> lock(irq_vec_mutex);
        irq_vec.push_back(ret);
    }

    return h;
}

gvar_ht global_t::define_var(pstring_t pstring, ideps_map_t&& ideps, 
                             src_type_t src_type, std::pair<group_vars_t*, group_vars_ht> group,
                             ast_node_t const* expr, std::unique_ptr<paa_def_t> paa_def, std::unique_ptr<mods_t> mods)
{
    gvar_t* ret;

    // Create the var
    gvar_ht h = { define(pstring, GLOBAL_VAR, std::move(ideps), [&](global_t& g)
    { 
        return gvar_ht::pool_emplace(ret, g, src_type, group.second, expr, std::move(paa_def), std::move(mods)).id;
    })};

    // Add it to the group
    if(group.first)
        group.first->add_gvar(h);
    else
    {
        std::lock_guard lock(gvar_t::m_groupless_gvars_lock);
        gvar_t::m_groupless_gvars.push_back(h);
    }
    
    return h;
}

const_ht global_t::define_const(pstring_t pstring, ideps_map_t&& ideps, 
                                src_type_t src_type, std::pair<group_data_t*, group_data_ht> group,
                                ast_node_t const* expr, std::unique_ptr<paa_def_t> paa_def,
                                std::unique_ptr<mods_t> mods)
{
    const_t* ret;

    // Create the const
    const_ht h = { define(pstring, GLOBAL_CONST, std::move(ideps), [&](global_t& g)
    { 
        return const_ht::pool_emplace(ret, g, src_type, group.second, expr, std::move(paa_def), std::move(mods)).id;
    })};

    // Add it to the group
    if(group.first)
        group.first->add_const(h);
    
    return h;
}

struct_ht global_t::define_struct(pstring_t pstring, ideps_map_t&& ideps,
                                  field_map_t&& fields)
                                
{
    struct_t* ret;

    // Create the struct
    struct_ht h = { define(pstring, GLOBAL_STRUCT, std::move(ideps), [&](global_t& g)
    { 
        return struct_ht::pool_emplace(ret, g, std::move(fields)).id;
    }) };
    
    return h;
}

charmap_ht global_t::define_charmap(
        pstring_t pstring, bool is_default, 
        string_literal_t const& characters, 
        string_literal_t const& sentinel,
        std::unique_ptr<mods_t> mods)
{
    charmap_t* ret;

    // Create the charmap
    charmap_ht h = { define(pstring, GLOBAL_CHARMAP, {}, [&](global_t& g)
    { 
        return charmap_ht::pool_emplace(ret, g, is_default, characters, sentinel, std::move(mods)).id;
    }) };

    return h;
}

global_t& global_t::default_charmap(pstring_t at)
{
    using namespace std::literals;
    static TLS global_t* result = nullptr;
    if(!result)
        result = &lookup_sourceless(at, "charmap"sv);
    return *result;
}

global_t& global_t::chrrom(pstring_t at)
{
    using namespace std::literals;
    static TLS global_t* result = nullptr;
    if(!result)
        result = &lookup_sourceless(at, "chrrom"sv);
    return *result;
}

global_t* global_t::chrrom() 
{ 
    using namespace std::literals;
    return lookup_sourceless("chrrom"sv); 
}


void global_t::init()
{
    assert(compiler_phase() == PHASE_INIT);
}

// This function isn't thread-safe.
// Call from a single thread only.
void global_t::parse_cleanup()
{
    assert(compiler_phase() == PHASE_PARSE_CLEANUP);

    // Verify groups are created:
    for(group_t const& group : group_ht::values())
        if(group.gclass() == GROUP_UNDEFINED)
            compiler_error(group.pstring(), fmt("Group name not in scope: %", group.name));

    // Verify globals are created:
    for(global_t& global : global_ht::values())
    {
        if(global.gclass() == GLOBAL_UNDEFINED)
        {
            if(global.m_pstring)
                compiler_error(global.pstring(), fmt("Name not in scope: %", global.name));
            else
                throw compiler_error_t(fmt("Name not in scope: %", global.name));
        }
    }

    // Validate groups and mods:
    for(fn_t const& fn : fn_ht::values())
    {
        for(mods_t const& mods : fn.def().mods)
            mods.validate_groups();

        if(fn.mods())
        {
            fn.mods()->validate_groups();

            if(global_t const* nmi = fn.mods()->nmi)
            {
                if(nmi->gclass() != GLOBAL_FN || nmi->impl<fn_t>().fclass != FN_NMI)
                {
                    throw compiler_error_t(
                        fmt_error(fn.global.pstring(), fmt("% is not a nmi function.", nmi->name))
                        + fmt_note(nmi->pstring(), "Declared here."));
                }
            }

            if(global_t const* irq = fn.mods()->irq)
            {
                if(irq->gclass() != GLOBAL_FN || irq->impl<fn_t>().fclass != FN_IRQ)
                {
                    throw compiler_error_t(
                        fmt_error(fn.global.pstring(), fmt("% is not an irq function.", irq->name))
                        + fmt_note(irq->pstring(), "Declared here."));
                }
            }
        }
    }

    // Determine group vars inits:
    for(group_vars_t& gv : group_vars_ht::values())
        gv.determine_has_init();

    // Setup NMI indexes:
    for(unsigned i = 0; i < nmis().size(); ++i)
        nmis()[i]->pimpl<nmi_impl_t>().index = i;

    // Setup IRQ indexes:
    for(unsigned i = 0; i < irqs().size(); ++i)
        irqs()[i]->pimpl<irq_impl_t>().index = i;
}

template<typename Fn>
void global_t::do_all(Fn const& fn)
{
    {
        std::lock_guard lock(ready_mutex);
        globals_left = global_ht::pool().size();
    }
    ready_cv.notify_all();

    // Spawn threads to compile in parallel:
    parallelize(compiler_options().num_threads,
    [&fn](std::atomic<bool>& exception_thrown)
    {
        ssa_pool::init();
        cfg_pool::init();

        while(!exception_thrown)
        {
            global_t* global = await_ready_global();

            if(!global)
                return;

            do global = fn(*global);
            while(global);
        }
    },
    []
    {
        {
            std::lock_guard lock(ready_mutex);
            globals_left = 0;
        }
        ready_cv.notify_all();
    });
}

// This function isn't thread-safe.
// Call from a single thread only.
void global_t::resolve_all()
{
    assert(compiler_phase() == PHASE_RESOLVE);

    do_all([&](global_t& g){ return g.resolve(nullptr); });
}

// This function isn't thread-safe.
// Call from a single thread only.
void global_t::precheck_all()
{
    assert(compiler_phase() == PHASE_PRECHECK);

    do_all([&](global_t& g){ return g.precheck(nullptr); });

    for(fn_t const* fn : modes())
        fn->precheck_finish_mode();
    for(fn_t const* fn : nmis())
        fn->precheck_finish_nmi_irq();
    for(fn_t const* fn : irqs())
        fn->precheck_finish_nmi_irq();

    // Verify fences:
    for(fn_t const* nmi : nmis())
        if(nmi->m_precheck_wait_nmi)
            compiler_error(nmi->global.pstring(), "Waiting for nmi inside nmi handler.");
    for(fn_t const* irq : irqs())
        if(irq->m_precheck_wait_nmi)
            compiler_error(irq->global.pstring(), "Waiting for nmi inside irq handler.");

    // Define 'm_precheck_parent_modes'
    for(fn_t* mode : modes())
    {
        fn_ht const mode_h = mode->handle();

        mode->m_precheck_calls.for_each([&](fn_ht call)
        {
            call->m_precheck_parent_modes.insert(mode_h);
        });

        mode->m_precheck_parent_modes.insert(mode_h);
    }

    // Allocate 'used_in_modes' for NMIs and IRQs:
    for(fn_t* nmi : nmis())
        nmi->pimpl<nmi_impl_t>().used_in_modes.alloc();
    for(fn_t* irq : irqs())
        irq->pimpl<irq_impl_t>().used_in_modes.alloc();

    // Then populate 'used_in_modes':
    for(fn_t* mode : modes())
    {
        if(fn_ht nmi = mode->mode_nmi())
            nmi->pimpl<nmi_impl_t>().used_in_modes.set(mode->handle().id);
        if(fn_ht irq = mode->mode_irq())
            irq->pimpl<irq_impl_t>().used_in_modes.set(mode->handle().id);
    }

    // Determine which rom procs each fn should have:

    for(fn_t* mode : modes())
    {
        assert(mode->fclass == FN_MODE);

        mode->m_precheck_calls.for_each([&](fn_ht call)
        {
            call->m_precheck_romv |= ROMVF_IN_MODE;
        });

        mode->m_precheck_romv |= ROMVF_IN_MODE;
    }

    for(fn_t* nmi : nmis())
    {
        assert(nmi->fclass == FN_NMI);

        nmi->m_precheck_calls.for_each([&](fn_ht call)
        {
            call->m_precheck_romv |= ROMVF_IN_NMI;
        });

        nmi->m_precheck_romv |= ROMVF_IN_NMI;
    }

    for(fn_t* irq : irqs())
    {
        assert(irq->fclass == FN_IRQ);

        irq->m_precheck_calls.for_each([&](fn_ht call)
        {
            call->m_precheck_romv |= ROMVF_IN_IRQ;
        });

        irq->m_precheck_romv |= ROMVF_IN_IRQ;
    }

    for(fn_t& fn : fn_ht::values())
    {
        // Allocate rom procs:
        assert(!fn.m_rom_proc);
        fn.m_rom_proc = rom_proc_ht::pool_make(romv_allocs_t{}, fn.m_precheck_romv, mod_test(fn.mods(), MOD_align));

        if(mod_test(fn.mods(), MOD_static))
            fn.m_rom_proc.safe().mark_rule(ROMR_STATIC);

        // Determine each 'm_precheck_called':
        if(fn.m_precheck_calls)
        {
            fn.m_precheck_calls.for_each([&](fn_ht call)
            {
                call->m_precheck_called += 1;

                if(fn.iasm)
                    call->mark_referenced_return(); // Used to disable inlining.
            });
        }
    }
}

// Not thread safe!
global_t* global_t::detect_cycle(global_t& global, idep_class_t pass, idep_class_t calc)
{
    if(global.m_ideps_left >= calc) // Re-use 'm_ideps_left' to track the DFS.
        return nullptr;

    if(global.m_ideps_left == -1)
        return &global;

    global.m_ideps_left = -1;

    for(auto const& pair : global.m_ideps)
    {
        assert(pair.second.calc);
        assert(pair.second.depends_on);

        // If we can calculate:
        if(pair.second.calc > calc)
            continue;

        // If the the idep was computed in a previous pass:
        if(pair.second.calc < pass || pair.second.depends_on < pass)
            continue;

        if(global_t* error = detect_cycle(*pair.first, pass, pair.second.depends_on))
        {
            if(error != &global)
            {
                detect_cycle_error_msgs.push_back(fmt_error(global.m_pstring, "Mutually recursive with:"));
                return error;
            }

            std::string msg = fmt_error(global.m_pstring,
                fmt("% has a recursive definition.", global.name));
            for(std::string const& str : detect_cycle_error_msgs)
                msg += str;

            detect_cycle_error_msgs.clear();
            throw compiler_error_t(std::move(msg));
        }
    }

    global.m_ideps_left = calc;

    return nullptr;
}

// This function isn't thread-safe.
// Call from a single thread only.
void global_t::count_members()
{
    unsigned total_members = 0;
    for(struct_t& s : struct_ht::values())
        total_members += s.count_members();

    gmember_ht::with_pool([total_members](auto& pool){ pool.reserve(total_members); });

    for(gvar_t& gvar : gvar_ht::values())
    {
        gvar.dethunkify(false);

        gmember_ht const begin = { gmember_ht::with_pool([](auto& pool){ return pool.size(); }) };

        unsigned const num = ::num_members(gvar.type());
        for(unsigned i = 0; i < num; ++i)
            gmember_ht::with_pool([&](auto& pool){ pool.emplace_back(gvar, pool.size()); });

        gmember_ht const end = { gmember_ht::with_pool([](auto& pool){ return pool.size(); }) };

        gvar.set_gmember_range(begin, end);
    }
}

// This function isn't thread-safe.
// Call from a single thread only.
void global_t::build_order()
{
    idep_class_t pass = {};
    switch(compiler_phase())
    {
    case PHASE_ORDER_RESOLVE:
        pass = IDEP_TYPE;
        break;
    case PHASE_ORDER_PRECHECK:
    case PHASE_ORDER_COMPILE:
        pass = IDEP_VALUE;
        break;
    default:
        assert(false);
    }

    // Detect cycles and clear 'm_iuses':
    for(global_t& global : global_ht::values())
    {
        detect_cycle(global, pass, pass);
        global.m_iuses.clear();
    }

    // Build the order:
    for(global_t& global : global_ht::values())
    {
        // 'm_ideps_left' was set by 'detect_cycle',
        // and holds the level of calculation required:
        idep_class_t const calc = idep_class_t(global.m_ideps_left.load());

        unsigned ideps_left = 0;
        for(auto const& pair : global.m_ideps)
        {
            // If we can calculate:
            if(pair.second.calc > calc)
                continue;

            // If the the idep was computed in a previous pass:
            if(pair.second.calc < pass || pair.second.depends_on < pass)
                continue;

            ++ideps_left;
            assert(pair.first != &global);
            pair.first->m_iuses.insert(&global);
        }

        global.m_ideps_left.store(ideps_left);

        if(ideps_left == 0)
            ready.push_back(&global);
    }

    assert(ready.size());
}

global_t* global_t::resolve(log_t* log)
{
    assert(compiler_phase() == PHASE_RESOLVE);

    dprint(log, "RESOLVING", name);
    delegate([](auto& g){ g.resolve(); });

    m_resolved = true;
    return completed();
}

global_t* global_t::precheck(log_t* log)
{
    assert(compiler_phase() == PHASE_PRECHECK);

    dprint(log, "PRECHECKING", name);
    delegate([](auto& g){ g.precheck(); });

    m_prechecked = true;
    return completed();
}

global_t* global_t::compile(log_t* log)
{
    assert(compiler_phase() == PHASE_COMPILE);

    dprint(log, "COMPILING", name, m_ideps.size());
    delegate([](auto& g){ g.compile(); });

    m_compiled = true;
    return completed();
}

global_t* global_t::completed()
{
    // OK! The global is done.
    // Now add all its dependents onto the ready list:

    global_t** newly_ready = ALLOCA_T(global_t*, m_iuses.size());
    global_t** newly_ready_end = newly_ready;

    for(global_t* iuse : m_iuses)
        if(--iuse->m_ideps_left == 0)
            *(newly_ready_end++) = iuse;

    std::size_t const newly_ready_size = newly_ready_end - newly_ready;

    if(newly_ready_size > 0)
        --newly_ready_end; // We'll return the last global, not insert it.

    unsigned new_globals_left;

    {
        std::lock_guard lock(ready_mutex);
        if(globals_left)
        {
            ready.insert(ready.end(), newly_ready, newly_ready_end);
            --globals_left;
        }
        new_globals_left = globals_left;
    }

    if(new_globals_left == 0 || newly_ready_size > 2)
        ready_cv.notify_all();
    else if(newly_ready_size == 2)
        ready_cv.notify_one();

    if(newly_ready_size > 0)
    {
        assert(*newly_ready_end);
        return *newly_ready_end;
    }
    else
        return nullptr;
}

global_t* global_t::await_ready_global()
{
    std::unique_lock<std::mutex> lock(ready_mutex);
    ready_cv.wait(lock, []{ return !ready.empty() || globals_left == 0; });

    if(globals_left == 0)
        return nullptr;
    
    global_t* ret = ready.back();
    ready.pop_back();
    return ret;
}

void global_t::compile_all()
{
    assert(compiler_phase() == PHASE_COMPILE);

    do_all([&](global_t& g){ return g.compile(nullptr); });
}

global_datum_t* global_t::datum() const
{
    switch(gclass())
    {
    case GLOBAL_VAR: return &impl<gvar_t>();
    case GLOBAL_CONST: return &impl<const_t>();
    default: return nullptr;
    }
}

std::vector<local_const_t> const* global_t::local_consts() const
{
    if(gclass() == GLOBAL_FN)
        return &impl<fn_t>().def().local_consts;
    if(auto* dat = datum())
        if(auto* def = dat->paa_def())
            return &def->local_consts;
    return nullptr;
}

//////////
// fn_t //
///////////

fn_t::fn_t(global_t& global, type_t type, fn_def_t&& fn_def, std::unique_ptr<mods_t> mods, 
           fn_class_t fclass, bool iasm) 
: modded_t(std::move(mods))
, global(global)
, fclass(fclass)
, iasm(iasm)
, m_type(std::move(type))
, m_def(std::move(fn_def)) 
{
    if(compiler_options().ir_info || mod_test(this->mods(), MOD_info))
        m_info_stream.reset(new std::stringstream());

    switch(fclass)
    {
    default:
        break;
    case FN_MODE: m_pimpl.reset(new mode_impl_t()); break;
    case FN_NMI:  m_pimpl.reset(new nmi_impl_t());  break;
    case FN_IRQ:  m_pimpl.reset(new irq_impl_t());  break;
    }
}

fn_ht fn_t::mode_nmi() const
{ 
    assert(fclass == FN_MODE); 
    assert(compiler_phase() > PHASE_PARSE_CLEANUP);
    return (mods() && mods()->nmi) ? mods()->nmi->handle<fn_ht>() : fn_ht{};
}

fn_ht fn_t::mode_irq() const
{ 
    assert(fclass == FN_MODE); 
    assert(compiler_phase() > PHASE_PARSE_CLEANUP);
    return (mods() && mods()->irq) ? mods()->irq->handle<fn_ht>() : fn_ht{};
}

unsigned fn_t::nmi_index() const
{
    assert(fclass == FN_NMI);
    assert(compiler_phase() > PHASE_PARSE_CLEANUP);
    return pimpl<nmi_impl_t>().index;
}

unsigned fn_t::irq_index() const
{
    assert(fclass == FN_IRQ);
    assert(compiler_phase() > PHASE_PARSE_CLEANUP);
    return pimpl<irq_impl_t>().index;
}

xbitset_t<fn_ht> const& fn_t::nmi_used_in_modes() const
{
    assert(fclass == FN_NMI);
    assert(compiler_phase() > PHASE_PRECHECK);
    return pimpl<nmi_impl_t>().used_in_modes;
}

xbitset_t<fn_ht> const& fn_t::irq_used_in_modes() const
{
    assert(fclass == FN_IRQ);
    assert(compiler_phase() > PHASE_PRECHECK);
    return pimpl<irq_impl_t>().used_in_modes;
}

bool fn_t::ct_pure() const
{
    switch(fclass)
    {
    case FN_CT:
        return true;
    case FN_FN:
        assert(global.compiled());
        return (ir_io_pure() 
                && ir_deref_groups().all_clear()
                && ir_reads().all_clear()
                && ir_writes().all_clear()
                && !ir_tests_ready());
    default:
        return false;
    }

}

void fn_t::calc_ir_bitsets(ir_t const* ir_ptr)
{
    xbitset_t<gmember_ht>  reads(0);
    xbitset_t<gmember_ht> writes(0);
    xbitset_t<group_vars_ht> group_vars(0);
    xbitset_t<group_ht> deref_groups(0);
    xbitset_t<fn_ht> calls(0);
    bool tests_ready = false;
    bool io_pure = true;
    bool fences = false;

    bool const is_static = mod_test(mods(), MOD_static);

    // Handle preserved groups
    for(auto const& fn_stmt : m_precheck_tracked->goto_modes)
    {
        if(mods_t const* goto_mods = fn_stmt.second.mods)
        {
            goto_mods->for_each_list_vars(MODL_PRESERVES, [&](group_vars_ht gv, pstring_t)
            {
                group_vars.set(gv.id);
                reads |= gv->gmembers();
            });
        }
    }

    // Handle 'employs':
    if(mods())
    {
        mods()->for_each_list(MODL_EMPLOYS, [&](group_ht g, pstring_t)
        {
            deref_groups.set(g.id); // Handles data and vars

            if(g->gclass() == GROUP_VARS)
            {
                auto const& gv = g->impl<group_vars_t>();
                reads  |= gv.gmembers();
                writes |= gv.gmembers();
            }
        });
    }

    if(iasm)
    {
        assert(!ir_ptr);
        assert(mods());
        assert(mods()->explicit_lists & MODL_EMPLOYS);

        io_pure = false;
        fences = true;
        group_vars = m_precheck_group_vars;

        for(auto const& pair : precheck_tracked().calls)
        {
            fn_t const& callee = *pair.first;
            reads   |= callee.ir_reads();
            writes  |= callee.ir_writes();
            calls   |= callee.ir_calls();
            calls.set(pair.first.id);
        }
    }
    else
    {
        assert(ir_ptr);
        ir_t const& ir = *ir_ptr;

        // Iterate the IR looking for reads and writes
        for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it; ++cfg_it)
        for(ssa_ht ssa_it = cfg_it->ssa_begin(); ssa_it; ++ssa_it)
        {
            if(ssa_flags(ssa_it->op()) & SSAF_IO_IMPURE)
                io_pure = false;

            if(ssa_it->op() == SSA_ready)
                tests_ready = true;

            if(ssa_it->op() == SSA_fn_call)
            {
                fn_ht const callee_h = get_fn(*ssa_it);
                fn_t const& callee = *callee_h;

                reads      |= callee.ir_reads();
                writes     |= callee.ir_writes();
                group_vars |= callee.ir_group_vars();
                calls      |= callee.ir_calls();
                fences     |= callee.ir_fences();
                io_pure    &= callee.ir_io_pure();
                calls.set(callee_h.id);
            }

            if(ssa_flags(ssa_it->op()) & SSAF_WRITE_GLOBALS)
            {
                for_each_written_global(ssa_it,
                [&](ssa_value_t def, locator_t loc)
                {
                    if(loc.lclass() == LOC_GMEMBER)
                    {
                        gmember_t const& written = *loc.gmember();
                        assert(written.gvar.global.gclass() == GLOBAL_VAR);

                        // Writes only have effect if they're not writing back a
                        // previously read value.
                        // TODO: verify this is correct
                        if(!def.holds_ref()
                           || def->op() != SSA_read_global
                           || def->input(1).locator() != loc)
                        {
                            writes.set(written.index);
                            if(written.gvar.group_vars)
                                group_vars.set(written.gvar.group_vars.id);
                        }
                    }
                });
            }
            else if(ssa_it->op() == SSA_read_global)
            {
                assert(ssa_it->input_size() == 2);
                locator_t loc = ssa_it->input(1).locator();

                if(loc.lclass() == LOC_GMEMBER)
                {
                    gmember_t const& read = *loc.gmember();
                    assert(read.gvar.global.gclass() == GLOBAL_VAR);

                    // Reads only have effect if something actually uses them:
                    for(unsigned i = 0; i < ssa_it->output_size(); ++i)
                    {
                        auto oe = ssa_it->output_edge(i);
                        // TODO: verify this is correct
                        if(!is_locator_write(oe) || oe.handle->input(oe.index + 1) != loc)
                        {
                            reads.set(read.index);
                            if(read.gvar.group_vars)
                                group_vars.set(read.gvar.group_vars.id);
                            break;
                        }
                    }
                }
            }

            if(ssa_flags(ssa_it->op()) & SSAF_INDEXES_PTR)
            {
                using namespace ssai::rw_ptr;

                io_pure = false;

                type_t const ptr_type = ssa_it->input(PTR).type();
                passert(is_ptr(ptr_type.name()), ssa_it->input(PTR));

                unsigned const size = ptr_type.group_tail_size();
                for(unsigned i = 0; i < size; ++i)
                {
                    // Set 'deref_groups':
                    group_ht const h = ptr_type.group(i);
                    deref_groups.set(h.id);

                    // Also update 'ir_group_vars':
                    group_t const& group = *h;
                    if(group.gclass() == GROUP_VARS)
                        group_vars.set(group.handle<group_vars_ht>().id);
                }
            }

            if(ssa_flags(ssa_it->op()) & SSAF_FENCE)
                fences = true;

            if(ssa_flags(ssa_it->op()) & SSAF_BANK_INPUT)
            {
                using namespace ssai::rw_ptr;
                ssa_value_t const bank = ssa_it->input(BANK);

                if(bank && is_static && mapper().bankswitches())
                    compiler_error(global.pstring(), "Function cannot be static if it bankswitches.");
            }
        }
    }

    m_ir_writes = std::move(writes);
    m_ir_reads  = std::move(reads);
    m_ir_group_vars = std::move(group_vars);
    m_ir_calls = std::move(calls);
    m_ir_deref_groups = std::move(deref_groups);
    m_ir_tests_ready = tests_ready;
    m_ir_io_pure = io_pure;
    m_ir_fences = fences;
}

void fn_t::assign_lvars(lvars_manager_t&& lvars)
{
    assert(compiler_phase() == PHASE_COMPILE);
    m_lvars = std::move(lvars);
    for(auto& vec : m_lvar_spans)
    {
        vec.clear();
        vec.resize(m_lvars.num_this_lvars());
    }
}

void fn_t::assign_lvar_span(romv_t romv, unsigned lvar_i, span_t span)
{
    assert(lvar_i < m_lvar_spans[romv].size()); 
    assert(!m_lvar_spans[romv][lvar_i]);
    assert(precheck_romv() & (1 << romv));

    m_lvar_spans[romv][lvar_i] = span;
    assert(lvar_span(romv, lvar_i) == span);
}

span_t fn_t::lvar_span(romv_t romv, int lvar_i) const
{
    assert(lvar_i < int(m_lvars.num_all_lvars()));

    if(lvar_i < 0)
        return {};

    if(lvar_i < int(m_lvars.num_this_lvars()))
        return m_lvar_spans[romv][lvar_i];

    locator_t const loc = m_lvars.locator(lvar_i);
    if(lvars_manager_t::is_call_lvar(handle(), loc))
    {
        int index = loc.fn()->m_lvars.index(loc);

        if(!loc.fn()->m_lvars.is_lvar(index))
            return {};

        return loc.fn()->lvar_span(romv, index);
    }

    throw std::runtime_error("Unknown lvar span");
}

span_t fn_t::lvar_span(romv_t romv, locator_t loc) const
{
    return lvar_span(romv, m_lvars.index(loc));
}

static void _resolve_local_consts(global_ht global, std::vector<local_const_t>& local_consts, fn_t* fn = nullptr)
{
    for(unsigned i = 0; i < local_consts.size(); ++i)
    {
        auto& c = local_consts[i];

        if(c.expr)
        {
            c.decl.src_type.type = ::dethunkify(c.decl.src_type, true);

            c.value = interpret_local_const(
                c.decl.name, fn, *c.expr, 
                c.decl.src_type.type, local_consts.data()).value;
        }
        else
        {
            locator_t const label = locator_t::named_label(global, i);

            if(is_banked_ptr(c.type().name()))
                c.value = { label, label.with_is(IS_BANK) };
            else
                c.value = { label };

            assert(c.value.size() == num_members(c.type()));
        }
    }
}

void fn_t::resolve()
{
    // Dethunkify the fn type:
    {
        type_t* types = ALLOCA_T(type_t, def().num_params + 1);
        for(unsigned i = 0; i != def().num_params; ++i)
            types[i] = ::dethunkify(def().local_vars[i].decl.src_type, true);
        types[def().num_params] = ::dethunkify(def().return_type, true);
        m_type = type_t::fn(types, types + def().num_params + 1);
    }

    if(fclass == FN_CT)
        goto resolve_ct;

    // Finish up types:
    for(unsigned i = 0; i < def().num_params; ++i)
    {
        auto const& decl = def().local_vars[i].decl;
        if(is_ct(decl.src_type.type))
            compiler_error(decl.src_type.pstring, fmt("Function must be declared as ct to use type %.", decl.src_type.type));
    }

    if(is_ct(def().return_type.type))
        compiler_error(def().return_type.pstring, fmt("Function must be declared as ct to use type %.", def().return_type.type));

    if(iasm)
    {
        if(!mods() || !(mods()->explicit_lists & MODL_EMPLOYS))
            compiler_error(global.pstring(), "Missing employs modifier.");
    }

    // Resolve local constants:
resolve_ct:
    _resolve_local_consts(global.handle(), m_def.local_consts, this);
}

void fn_t::precheck()
{
    if(fclass == FN_CT)
        return; // Nothing to do!

    if(iasm)
    {
        assert(def().stmts.size() == 2);
        assert(def().stmts[0].name == STMT_EXPR);
        assert(!m_precheck_tracked);
        m_precheck_tracked.reset(new precheck_tracked_t(build_tracked(*this, def().local_consts.data())));
    }
    else
    {
        // Run the evaluator to generate 'm_precheck_tracked':
        assert(!m_precheck_tracked);
        m_precheck_tracked.reset(new precheck_tracked_t(build_tracked(*this, def().local_consts.data())));
    }

    assert(m_precheck_tracked);
    calc_precheck_bitsets();
}

void fn_t::compile_iasm()
{
    assert(iasm);

    calc_ir_bitsets(nullptr);

    assert(def().stmts.size() == 2);
    assert(def().stmts[0].name == STMT_EXPR);
    assert(def().stmts[0].expr[0].token.type == lex::TOK_byte_block_proc);

    asm_proc_t proc = std::get<asm_proc_t>(interpret_byte_block(def().stmts[0].pstring, def().stmts[0].expr[0], 
                                                                this, def().local_consts.data()));
    proc.fn = handle();

    assign_lvars(lvars_manager_t(*this));

    proc.build_label_offsets();
    rom_proc().safe().assign(std::move(proc));
}

void fn_t::compile()
{
    log_t* log = nullptr;
    assert(compiler_phase() == PHASE_COMPILE);

    if(fclass == FN_CT)
        return; // Nothing to do!

    // Init 'fence_rw':
    if(precheck_fences())
    {
        m_fence_rw.alloc();
        for(fn_ht mode : precheck_parent_modes())
        {
            if(fn_ht nmi = mode->mode_nmi())
                m_fence_rw |= nmi->precheck_rw();
            if(fn_ht irq = mode->mode_irq())
                m_fence_rw |= irq->precheck_rw();
        }
        assert(m_fence_rw);
    }

    if(iasm)
        return compile_iasm();

    // Compile the FN.
    ssa_pool::clear();
    cfg_pool::clear();
    ir_t ir;
    build_ir(ir, *this);

    auto const save_graph = [&](ir_t& ir, char const* suffix)
    {
        if(!compiler_options().graphviz && !mod_test(mods(), MOD_graphviz))
            return;

        std::filesystem::create_directory("graphs/");

        std::ofstream ocfg(fmt("graphs/cfg__%__%.gv", global.name, suffix));
        if(ocfg.is_open())
            graphviz_cfg(ocfg, ir);

        std::ofstream ossa(fmt("graphs/ssa__%__%.gv", global.name, suffix));
        if(ossa.is_open())
            graphviz_ssa(ossa, ir);
    };

    auto const optimize_suite = [&](bool post_byteified)
    {
#define RUN_O(o, ...) do { if(o(__VA_ARGS__)) { \
    changed = true; \
    /*assert((std::printf("DID_O %s %s\n", global.name.c_str(), #o), true));*/ } \
    ir.assert_valid(); \
    } while(false)

        unsigned iter = 0;
        constexpr unsigned MAX_ITER = 100;
        bool changed;

        // Do this first, to reduce the size of the IR:
        o_remove_unused_ssa(log, ir);

        do
        {
            changed = false;

            save_graph(ir, fmt("pre_fork_%_%", post_byteified, iter).c_str());

            RUN_O(o_defork, log, ir);
            RUN_O(o_fork, log, ir);

            RUN_O(o_phis, log, ir);

            RUN_O(o_merge_basic_blocks, log, ir);

            RUN_O(o_remove_unused_arguments, log, ir, *this, post_byteified);

            save_graph(ir, fmt("pre_id_%_%", post_byteified, iter).c_str());
            RUN_O(o_identities, log, ir);
            save_graph(ir, fmt("post_id_%_%", post_byteified, iter).c_str());

            // 'o_loop' populates 'ai_prep', which feeds into 'o_abstract_interpret'.
            // Thus, they must occur sequentially.
            reset_ai_prep();
            save_graph(ir, fmt("pre_loop_%_%", post_byteified, iter).c_str());
            RUN_O(o_loop, log, ir, post_byteified);
            save_graph(ir, fmt("pre_ai_%_%", post_byteified, iter).c_str());
            RUN_O(o_abstract_interpret, log, ir, post_byteified);
            save_graph(ir, fmt("post_ai_%_%", post_byteified, iter).c_str());

            RUN_O(o_remove_unused_ssa, log, ir);

            save_graph(ir, fmt("pre_motion_%_%", post_byteified, iter).c_str());
            RUN_O(o_motion, log, ir);
            save_graph(ir, fmt("post_motion_%_%", post_byteified, iter).c_str());

            if(post_byteified)
            {
                // Once byteified, keep shifts out of the IR and only use rotates.
                changed |= shifts_to_rotates(ir, true);
            }

            // Enable this to debug:
            save_graph(ir, fmt("during_o_%", iter).c_str());
            ++iter;

            if(iter >= MAX_ITER)
                break;
        }
        while(changed);
    };

    save_graph(ir, "1_initial");
    ir.assert_valid();

    optimize_suite(false);
    save_graph(ir, "2_o1");

    // Set the global's 'read' and 'write' bitsets:
    calc_ir_bitsets(&ir);
    assert(ir_reads());

    // Convert switches:
    if(switch_partial_to_full(ir))
        optimize_suite(false);
    save_graph(ir, "3_switch");

    byteify(ir, *this);
    save_graph(ir, "4_byteify");
    ir.assert_valid();

    optimize_suite(true);
    save_graph(ir, "5_o2");

    std::size_t const proc_size = code_gen(log, ir, *this);
    save_graph(ir, "6_cg");

    // Calculate inline-ability
    assert(m_always_inline == false);
    if(fclass == FN_FN && !mod_test(mods(), MOD_inline, false))
    {
        if(referenced())
        {
            m_always_inline = false;
            if(mod_test(mods(), MOD_inline, true))
                compiler_warning(global.pstring(), fmt("Unable to inline % as its being addressed.", global.name));
        }
        else if(mod_test(mods(), MOD_inline, true))
            m_always_inline = true;
        else if(precheck_called() == 1)
        {
            if(proc_size < INLINE_SIZE_ONCE)
                m_always_inline = true;
        }
        else if(proc_size < INLINE_SIZE_LIMIT)
        {
            bool const no_banks = ir_deref_groups().for_each_test([&](group_ht group) -> bool
            {
                return group->gclass() != GROUP_DATA || !group->impl<group_data_t>().once;
            });

            if(no_banks)
            {
                unsigned call_cost = 0;

                m_lvars.for_each_lvar(true, [&](locator_t loc, unsigned)
                {
                    if(loc.lclass() == LOC_ARG || loc.lclass() == LOC_RETURN)
                    {
                        assert(loc.fn() == handle());
                        ++call_cost;
                    }
                });

                constexpr unsigned CALL_PENALTY = 3;

                if(proc_size < INLINE_SIZE_GOAL + (call_cost * CALL_PENALTY))
                    m_always_inline = true;
            }
        }
    }
}

void fn_t::precheck_finish_mode() const
{
    assert(fclass == FN_MODE);

    auto& pimpl = this->pimpl<mode_impl_t>();

    for(auto const& pair : pimpl.incoming_preserved_groups)
    {
        if(pair.first->gclass() != GROUP_VARS)
            continue;

        if(!precheck_group_vars().test(pair.first->impl_id()))
        {
            std::string groups;
            precheck_group_vars().for_each([&groups](group_vars_ht gv)
            {
                groups += gv->group.name;
            });
            if(groups.empty())
                groups = "(no groups)";
            else
                groups = "vars " + groups;

            compiler_warning(
                fmt_warning(pair.second, 
                    fmt("Preserving % has no effect as mode % is excluding it.", 
                        pair.first->name, global.name))
                + fmt_note(global.pstring(), fmt("% includes: %", 
                                                 global.name, groups)), false);
        }
    }
}

void fn_t::precheck_finish_nmi_irq() const
{
    assert(fclass == FN_NMI || fclass == FN_IRQ);

    auto const first_goto_mode = [](fn_t const& fn) -> pstring_t
    {
        if(fn.precheck_tracked().goto_modes.empty())
            return {};
        return fn.precheck_tracked().goto_modes.begin()->second.pstring;
    };

    if(pstring_t pstring = first_goto_mode(*this))
        compiler_error(pstring, fmt("goto mode inside %.", fn_class_keyword(fclass)));

    m_precheck_calls.for_each([&](fn_ht call)
    {
        if(pstring_t pstring = first_goto_mode(*call))
        {
            throw compiler_error_t(
                fmt_error(pstring, fmt("goto mode reachable from % %", fn_class_keyword(fclass), global.name))
                + fmt_note(global.pstring(), fmt("% declared here.", global.name)));
        }
    });
}

void fn_t::calc_precheck_bitsets()
{
    assert(compiler_phase() == PHASE_PRECHECK);

    // Set 'wait_nmi' and 'fences':
    m_precheck_wait_nmi |= m_precheck_tracked->wait_nmis.size() > 0;

    if(iasm)
        m_precheck_fences = true;
    else
    {
        m_precheck_fences |= m_precheck_tracked->fences.size() > 0;
        m_precheck_fences |= m_precheck_wait_nmi;
    }

    unsigned const gv_bs_size = group_vars_ht::bitset_size();

    assert(!m_precheck_group_vars);
    assert(!m_precheck_rw);
    assert(!m_precheck_calls);

    m_precheck_group_vars.alloc();
    m_precheck_rw.alloc();
    m_precheck_calls.alloc();

    // For efficiency, we'll convert the mod groups into a bitset.
    bitset_uint_t* temp_bs = ALLOCA_T(bitset_uint_t, gv_bs_size);
    bitset_uint_t* mod_group_vars = ALLOCA_T(bitset_uint_t, gv_bs_size);

    bitset_clear_all(gv_bs_size, mod_group_vars);
    if(mods() && (mods()->explicit_lists & MODL_VARS))
    {
        mods()->for_each_list_vars(MODL_VARS, [&](group_vars_ht gv, pstring_t)
        {
            bitset_set(mod_group_vars, gv.id);
        });

        bitset_or(gv_bs_size, m_precheck_group_vars.data(), mod_group_vars);
    }

    if(mods() && (mods()->explicit_lists & MODL_EMPLOYS))
    {
        mods()->for_each_list_vars(MODL_EMPLOYS, [&](group_vars_ht gv, pstring_t)
        {
            bitset_set(m_precheck_group_vars.data(), gv.id);
        });
    }

    auto const group_vars_to_string = [gv_bs_size](bitset_uint_t const* bs)
    {
        std::string str;
        bitset_for_each<group_vars_ht>(gv_bs_size, bs, [&str](auto gv)
            { str += gv->group.name; });
        return str;
    };

    auto const group_map_to_string = [this](mod_list_t lists)
    {
        std::string str;
        for(auto const& pair : mods()->lists)
            if(pair.second.lists & lists)
                str += pair.first->name;
        return str;
    };

    // Handle accesses through pointers:
    for(auto const& pair : m_precheck_tracked->deref_groups)
    {
        auto const error = [&](group_class_t gclass, mod_list_t lists)
        {
            type_t const type = pair.second.type;

            char const* const keyword = group_class_keyword(gclass);

            std::string const msg = fmt(
                "% access requires groups that are excluded from % (% %).",
                type, global.name, keyword, group_map_to_string(lists));

            std::string missing = "";
            for(unsigned i = 0; i < type.group_tail_size(); ++i)
            {
                group_ht const group = type.group(i);
                if(group->gclass() == gclass && !mods()->in_lists(lists, group))
                    missing += group->name;
            }

            throw compiler_error_t(
                fmt_error(pair.second.pstring, msg)
                + fmt_note(fmt("Excluded groups: % %", keyword, missing)));
        };

        if(pair.first->gclass() == GROUP_VARS)
        {
            if(mods() && (mods()->explicit_lists & MODL_VARS) && !mods()->in_lists(MODL_VARS, pair.first))
                error(GROUP_VARS, MODL_VARS);

            m_precheck_group_vars.set(pair.first->impl_id());
        }
        else if(pair.first->gclass() == GROUP_DATA)
        {
            if(mods() && (mods()->explicit_lists & MODL_DATA) && !mods()->in_lists(MODL_DATA, pair.first))
                error(GROUP_DATA, MODL_DATA);
        }
    }

    // Handle accesses through goto modes:
    for(auto const& fn_stmt : m_precheck_tracked->goto_modes)
    {
        pstring_t pstring = fn_stmt.second.pstring;
        mods_t const* goto_mods = fn_stmt.second.mods;

        if(!goto_mods || !(goto_mods->explicit_lists & MODL_PRESERVES))
            compiler_error(pstring ? pstring : global.pstring(), 
                           "Missing preserves modifier.");

        if(!goto_mods)
            continue;

        // Track incoming, for the called mode: 
        auto& call_pimpl = fn_stmt.first->pimpl<mode_impl_t>();
        {
            std::lock_guard<std::mutex> lock(call_pimpl.incoming_preserved_groups_mutex);
            goto_mods->for_each_list(MODL_PRESERVES, [&](group_ht g, pstring_t pstring)
            {
                call_pimpl.incoming_preserved_groups.emplace(g, pstring);
            });
        }

        // Handle our own groups:
        goto_mods->for_each_list(MODL_PRESERVES, [&](group_ht g, pstring_t)
        {
            if(g->gclass() == GROUP_VARS)
            {
                if(mods() && (mods()->explicit_lists & MODL_PRESERVES) && !mods()->in_lists(MODL_PRESERVES, g))
                {
                    std::string const msg = fmt(
                        "Preserved groups are excluded from % (vars %).",
                        global.name, group_map_to_string(MODL_VARS));

                    std::string missing = "";
                    goto_mods->for_each_list(MODL_PRESERVES, [&](group_ht g, pstring_t)
                    {
                        if(g->gclass() == GROUP_VARS && !mods()->in_lists(MODL_PRESERVES, g))
                            missing += g->name;
                    });

                    throw compiler_error_t(
                        fmt_error(pstring, msg)
                        + fmt_note(fmt("Excluded groups: vars %", missing)));
                }

                m_precheck_group_vars.set(g->impl_id());
            }
        });
    }

    // Handle direct dependencies.

    {
        auto const error = [&](global_t const& idep, 
                               std::string const& dep_group_string, 
                               std::string const& missing)
        {
            std::string const msg = fmt(
                "% (vars %) requires groups that are excluded from % (vars %).",
                idep.name, dep_group_string, global.name, 
                group_vars_to_string(mod_group_vars));

            if(pstring_t const pstring = def().find_global(&idep))
            {
                throw compiler_error_t(
                    fmt_error(pstring, msg)
                    + fmt_note(fmt("Excluded groups: vars %", missing)));
            }
            else // This shouldn't occur, but just in case...
                compiler_error(global.pstring(), msg);
        };

        for(auto const& pair : m_precheck_tracked->gvars_used)
        {
            gvar_ht const gvar = pair.first;
            m_precheck_rw.set_n(gvar->begin().id, gvar->num_members());

            if(group_ht const group = gvar->group())
            {
                if(mods() && (mods()->explicit_lists & MODL_VARS) && !mods()->in_lists(MODL_VARS, group))
                    error(pair.first->global, group->name, group->name);

                m_precheck_group_vars.set(group->impl_id());
            }
        }

        for(auto const& pair : m_precheck_tracked->calls)
        {
            fn_t& call = *pair.first;

            passert(call.fclass != FN_CT, global.name, call.global.name);
            passert(call.fclass != FN_MODE, global.name, call.global.name);
            passert(call.m_precheck_group_vars, global.name, call.global.name);
            passert(call.m_precheck_rw, global.name, call.global.name);

            bitset_copy(gv_bs_size, temp_bs, call.m_precheck_group_vars.data());
            bitset_difference(gv_bs_size, temp_bs, mod_group_vars);

            if(mods() && (mods()->explicit_lists & MODL_VARS) && !bitset_all_clear(gv_bs_size, temp_bs))
            {
                error(call.global,
                      group_vars_to_string(call.precheck_group_vars().data()),
                      group_vars_to_string(temp_bs));
            }

            m_precheck_group_vars |= call.m_precheck_group_vars;
            m_precheck_rw |= call.m_precheck_rw;

            // Calls
            m_precheck_calls.set(pair.first.id);
            m_precheck_calls |= call.m_precheck_calls;

            // 'wait_nmi' and 'fences'
            m_precheck_fences |= call.m_precheck_fences;
            m_precheck_wait_nmi |= call.m_precheck_wait_nmi;
        }
    }
}

void fn_t::mark_referenced_return()
{
    std::uint64_t expected = m_referenced.load();
    while(!m_referenced.compare_exchange_weak(expected, expected | 1));
    assert(referenced_return());
}

void fn_t::mark_referenced_param(unsigned param)
{
    assert(param < 63);
    std::uint64_t const mask = 0b10ull << param;
    assert(mask);
    std::uint64_t expected = m_referenced.load();
    while(!m_referenced.compare_exchange_weak(expected, expected | mask));
    assert(m_referenced.load() & mask);
}

void fn_t::for_each_referenced_locator(std::function<void(locator_t)> const& fn) const
{
    type_t const return_type = type().return_type();

    if(return_type.name() != TYPE_VOID && referenced_return())
    {
        unsigned const num_members = ::num_members(return_type);

        for(unsigned j = 0; j < num_members; ++j)
        {
            type_t const member_type = ::member_type(return_type, j);
            unsigned const num_atoms = ::num_atoms(member_type, 0);

            for(unsigned k = 0; k < num_atoms; ++k)
                fn(locator_t::ret(handle(), j, k));
        }
    }

    for_each_referenced_param_locator(fn);
}

void fn_t::for_each_referenced_param_locator(std::function<void(locator_t)> const& fn) const
{
    bitset_for_each(referenced_params(), [&](unsigned param)
    {
        var_decl_t const& decl = def().local_vars[param].decl;
        type_t const param_type = decl.src_type.type;
        unsigned const num_members = ::num_members(param_type);

        for(unsigned j = 0; j < num_members; ++j)
        {
            type_t const member_type = ::member_type(param_type, j);
            unsigned const num_atoms = ::num_atoms(member_type, 0);

            for(unsigned k = 0; k < num_atoms; ++k)
                fn(locator_t::arg(handle(), param, j, k));
        }
    });
}

locator_t fn_t::new_asm_goto_mode(fn_ht fn, unsigned label, pstring_t pstring, mods_t const* mods)
{
    if(!m_asm_goto_modes)
        m_asm_goto_modes.reset(new std::vector<asm_goto_mode_t>());
    auto& vec = *m_asm_goto_modes;

    locator_t const loc = locator_t::asm_goto_mode(handle(), vec.size());

    vec.push_back(asm_goto_mode_t{ 
        .fn = fn, 
        .label = label, 
        .pstring = pstring,
        .rom_proc = rom_proc_ht::pool_make(romv_allocs_t{}, m_precheck_romv, false)
    });

    if(mods)
    {
        mods->for_each_list_vars(MODL_PRESERVES, [&](group_vars_ht gv, pstring_t)
        {
            vec.back().preserves.insert(gv);
        });
    }

    return loc;
}

rom_proc_ht fn_t::asm_goto_mode_rom_proc(unsigned i) const
{
    assert(compiler_phase() > PHASE_COMPILE);
    assert(m_asm_goto_modes);
    assert(i < m_asm_goto_modes->size());

    return (*m_asm_goto_modes)[i].rom_proc;
}

// Not thread safe.
void fn_t::implement_asm_goto_modes()
{
    for(fn_t& fn : fn_ht::values())
    {
        if(auto* vec = fn.m_asm_goto_modes.get())
        for(auto& d : *vec)
        {
            asm_proc_t proc;
            int const iasm_child = proc.add_pstring(d.pstring);

            // Reset global variables:
            bool did_reset_nmi = false;
            bool did_reset_irq = false;
            d.fn->precheck_group_vars().for_each([&](group_vars_ht gv)
            {
                if(!gv->has_init())
                    return;

                if(!d.preserves.count(gv))
                {
                    if(!did_reset_nmi)
                    {
                        // Reset the nmi handler until we've reset all group vars.
                        proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = locator_t::const_byte(0) });
                        proc.push_inst({ .op = STY_ABSOLUTE, .iasm_child = iasm_child, .arg = locator_t::runtime_ram(RTRAM_nmi_index) });
                        did_reset_nmi = true;
                    }

                    if(!did_reset_irq)
                    {
                        // Reset the irq handler until we've reset all group vars.
                        proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = locator_t::const_byte(0) });
                        proc.push_inst({ .op = STY_ABSOLUTE, .iasm_child = iasm_child, .arg = locator_t::runtime_ram(RTRAM_irq_index) });
                        did_reset_nmi = true;
                    }

                    locator_t const loc = locator_t::reset_group_vars(gv);
                    proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = loc.with_is(IS_BANK) });
                    proc.push_inst({ .op = mapper().bankswitches() ? BANKED_Y_JSR : JSR_ABSOLUTE, .iasm_child = iasm_child, .arg = loc });
                }
            });

            bool same_nmi = true;
            bool same_irq = true;
            for(fn_ht mode : fn.precheck_parent_modes())
            {
                same_nmi &= mode->mode_nmi() == d.fn->mode_nmi();
                same_irq &= mode->mode_irq() == d.fn->mode_irq();
            }

            // Set the NMI
            if(did_reset_nmi || !same_nmi)
            {
                proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = locator_t::nmi_index(d.fn->mode_nmi()) });
                proc.push_inst({ .op = STY_ABSOLUTE, .iasm_child = iasm_child, .arg = locator_t::runtime_ram(RTRAM_nmi_index) });
            }

            // Set the IRQ
            if(did_reset_irq || !same_irq)
            {
                proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = locator_t::irq_index(d.fn->mode_irq()) });
                proc.push_inst({ .op = STY_ABSOLUTE, .iasm_child = iasm_child, .arg = locator_t::runtime_ram(RTRAM_irq_index) });
            }

            // Set the IRQ
            if(did_reset_nmi || !same_nmi)
            {
                proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = locator_t::nmi_index(d.fn->mode_nmi()) });
                proc.push_inst({ .op = STY_ABSOLUTE, .iasm_child = iasm_child, .arg = locator_t::runtime_ram(RTRAM_nmi_index) });
            }

            // Do the jump
            locator_t const loc = locator_t::fn(d.fn, d.label);
            proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = iasm_child, .arg = loc.with_is(IS_BANK) });
            proc.push_inst({ .op = mapper().bankswitches() ? BANKED_Y_JMP : JMP_ABSOLUTE, .iasm_child = iasm_child, .arg = loc });

            // Assign the proc:
            d.rom_proc.safe().assign(std::move(proc));
        }
    }
}

////////////////////
// global_datum_t //
////////////////////

void global_datum_t::dethunkify(bool full)
{
    assert(compiler_phase() == PHASE_RESOLVE || compiler_phase() == PHASE_COUNT_MEMBERS);
    m_src_type.type = ::dethunkify(m_src_type, full);
}

void global_datum_t::resolve()
{
    assert(compiler_phase() == PHASE_RESOLVE);
    dethunkify(true);

    if(!init_expr)
        return;

    if(m_src_type.type.name() == TYPE_PAA)
    {
        passert(paa_def(), global.name);
        _resolve_local_consts(global.handle(), m_def->local_consts);

        auto data = interpret_byte_block(global.pstring(), *init_expr, nullptr, paa_def()->local_consts.data());
        std::size_t data_size = 0;

        if(auto const* proc = std::get_if<asm_proc_t>(&data))
            data_size = proc->size();
        else if(auto const* vec = std::get_if<loc_vec_t>(&data))
            data_size = vec->size();
        else
            assert(false);

        unsigned const def_length = m_src_type.type.array_length();
        if(def_length && def_length != data_size)
             compiler_error(m_src_type.pstring, fmt("Length of data (%) does not match its type %.", data_size, m_src_type.type));

        m_src_type.type.set_array_length(data_size);
        std::visit([this](auto&& v){ paa_init(std::move(v)); }, data);
    }
    else
    {
        rpair_t rpair = interpret_expr(global.pstring(), *init_expr, m_src_type.type);
        m_src_type.type = std::move(rpair.type); // Handles unsized arrays
        rval_init(std::move(rpair.value));
    }
}

void global_datum_t::precheck()
{
    assert(compiler_phase() == PHASE_PRECHECK);
}

void global_datum_t::compile()
{
    assert(compiler_phase() == PHASE_COMPILE);
}

////////////
// gvar_t //
////////////

group_ht gvar_t::group() const { return group_vars ? group_vars->group.handle() : group_ht{}; }

void gvar_t::paa_init(loc_vec_t&& paa) { m_init_data = std::move(paa); }
void gvar_t::paa_init(asm_proc_t&& proc) { m_init_data = std::move(proc); }

void gvar_t::rval_init(rval_t&& rval)
{
    m_rval = std::move(rval);

    loc_vec_t vec;
    append_locator_bytes(vec, m_rval, m_src_type.type, global.pstring());
    m_init_data = std::move(vec);
}

void gvar_t::set_gmember_range(gmember_ht begin, gmember_ht end)
{
    assert(compiler_phase() == PHASE_COUNT_MEMBERS);
    m_begin_gmember = begin;
    m_end_gmember = end;
}

void gvar_t::for_each_locator(std::function<void(locator_t)> const& fn) const
{
    assert(compiler_phase() > PHASE_COMPILE);

    for(gmember_ht h = begin(); h != end(); ++h)
    {
        unsigned const num = num_atoms(h->type(), 0);
        for(unsigned atom = 0; atom < num; ++atom)
            fn(locator_t::gmember(h, atom));
    }
}

void gvar_t::relocate_init_data(std::uint16_t addr)
{
    if(asm_proc_t* proc = std::get_if<asm_proc_t>(&m_init_data))
    {
        proc->relocate(locator_t::addr(addr));
        m_init_data = proc->loc_vec();
    }
}

///////////////
// gmember_t //
///////////////

void gmember_t::alloc_spans()
{
    assert(compiler_phase() == PHASE_ALLOC_RAM);
    assert(m_spans.empty());
    m_spans.resize(num_atoms(type(), 0));
}

locator_t const* gmember_t::init_data(unsigned atom, loc_vec_t const& vec) const
{
    unsigned const size = init_size();
    unsigned const offset = ::member_offset(gvar.type(), member());
    return vec.data() + offset + (atom * size);
}

locator_t const* gmember_t::init_data(unsigned atom) const
{
    return init_data(atom, std::get<loc_vec_t>(gvar.init_data()));
}

std::size_t gmember_t::init_size() const
{
    return ::num_offsets(type());
}

bool gmember_t::zero_init(unsigned atom) const
{
    if(!gvar.init_expr)
        return false;

    if(loc_vec_t const* vec = std::get_if<loc_vec_t>(&gvar.init_data()))
    {
        std::size_t const size = init_size();
        locator_t const* data = init_data(atom, *vec);

        for(unsigned i = 0; i < size; ++i)
            if(!data[i].eq_const(0))
                return false;

        return true;
    }

    return false;
}

/////////////
// const_t //
/////////////

group_ht const_t::group() const { return group_data ? group_data->group.handle() : group_ht{}; }

void const_t::paa_init(loc_vec_t&& vec)
{
    rom_rule_t rule = ROMR_NORMAL;
    if(mod_test(mods(), MOD_dpcm))
        rule = ROMR_DPCM;
    else if(!group_data)
        rule = ROMR_STATIC;

    m_rom_array = rom_array_t::make(std::move(vec), mod_test(mods(), MOD_align), rule, group_data);
    assert(m_rom_array);
}

void const_t::paa_init(asm_proc_t&& proc)
{
    try
    {
        proc.absolute_to_zp();
        proc.build_label_offsets();
        //proc.write_assembly(std::cout, ROMV_MODE);
        proc.relocate(locator_t::gconst(handle()));
    }
    catch(relocate_error_t const& e)
    {
        compiler_error(global.pstring(), e.what());
    }

    assert(m_def);
    auto& def = *m_def;

    // Copy the offsets from 'proc' into 'def.offsets',
    // as proc won't stick around.
    def.offsets.resize(def.local_consts.size(), 0);
    for(auto const& pair : proc.labels)
        if(pair.first.lclass() == LOC_NAMED_LABEL && pair.first.global() == global.handle())
            def.offsets[pair.first.data()] = pair.second.offset;

    paa_init(proc.loc_vec());
}

void const_t::rval_init(rval_t&& rval)
{
    m_rval = std::move(rval);
}

//////////////
// struct_t //
//////////////

unsigned struct_t::count_members()
{
    assert(compiler_phase() == PHASE_COUNT_MEMBERS);

    if(m_num_members != UNCOUNTED)
        return m_num_members;

    unsigned count = 0;

    for(unsigned i = 0; i < fields().size(); ++i)
    {
        type_t type = const_cast<type_t&>(field(i).type()) = dethunkify(field(i).decl.src_type, false);

        if(is_tea(type.name()))
            type = type.elem_type();

        if(type.name() == TYPE_STRUCT)
        {
            struct_t& s = const_cast<struct_t&>(type.struct_());
            s.count_members();
            assert(s.m_num_members != UNCOUNTED);
            count += s.m_num_members;
        }
        else
        {
            assert(!is_aggregate(type.name()));
            ++count;
        }
    }

    return m_num_members = count;
}

void struct_t::resolve()
{
    assert(compiler_phase() == PHASE_RESOLVE);

    // Dethunkify
    for(unsigned i = 0; i < fields().size(); ++i)
        const_cast<type_t&>(field(i).type()) = dethunkify(field(i).decl.src_type, true);

    gen_member_types(*this, 0);
}

void struct_t::precheck() { assert(compiler_phase() == PHASE_PRECHECK); }
void struct_t::compile() { assert(compiler_phase() == PHASE_COMPILE); }

// Builds 'm_member_types', sets 'm_has_array_member', and dethunkifies the struct.
void struct_t::gen_member_types(struct_t const& s, unsigned tea_size)
{
    std::uint16_t offset = 0;
    
    for(unsigned i = 0; i < fields().size(); ++i)
    {
        type_t type = s.field(i).type();

        if(type.name() == TYPE_TEA)
        {
            tea_size = type.size();
            type = type.elem_type();
        }

        assert(!is_thunk(type.name()));

        if(type.name() == TYPE_STRUCT)
        {
            assert(&type.struct_() != &s);
            gen_member_types(type.struct_(), tea_size);
        }
        else
        {
            assert(!is_aggregate(type.name()));
            assert(tea_size <= 256);

            if(tea_size)
            {
                type = type_t::tea(type, tea_size);
                m_has_tea_member = true;
            }

            m_member_types.push_back(type);
            m_member_offsets.push_back(offset);

            offset += type.size_of();
        }
    }
}

///////////////
// charmap_t //
///////////////

charmap_t::charmap_t(global_t& global, bool is_default, 
                     string_literal_t const& characters, 
                     string_literal_t const& sentinel,
                     std::unique_ptr<mods_t> new_mods)
: modded_t(std::move(new_mods))
, global(global)
, is_default(is_default)
{
    auto const add_literal = [&](string_literal_t const& lit)
    {
        if(lit.string.empty())
            return;

        char const* ptr = lit.string.data();
        char const* char_begin = ptr;
        char32_t utf32 = escaped_utf8_to_utf32(lit.pstring, ptr);
        assert(ptr != char_begin);

        char const* end = lit.string.data() + lit.string.size();
        while(true)
        {
            assert(ptr);

            if(utf32 == SPECIAL_SLASH)
                compiler_error(lit.pstring, "Invalid '\\/' operator.");

            auto result = m_map.insert({ utf32, m_num_unique });

            if(!result.second)
                compiler_error(lit.pstring, fmt("Duplicate character: '%'", std::string_view(char_begin, ptr)));

            if(ptr == end)
            {
                ++m_num_unique;
                break;
            }

            char_begin = ptr;
            utf32 = escaped_utf8_to_utf32(ptr);

            if(utf32 == SPECIAL_SLASH)
            {
                char_begin = ptr;
                utf32 = escaped_utf8_to_utf32(ptr);
                if(!ptr)
                    compiler_error(lit.pstring, "Invalid '\\/' operator.");
            }
            else
                ++m_num_unique;
        }
    };

    add_literal(characters);

    if(m_num_unique > 256)
        compiler_error(global.pstring(), fmt("Too many characters (%) in charmap. Max: 256.", m_num_unique));

    if(m_num_unique == 0)
        compiler_error(global.pstring(), "Empty charmap");

    if(!sentinel.string.empty())
    {
        char const* ptr = sentinel.string.data();
        char32_t utf32 = escaped_utf8_to_utf32(ptr);

        if(ptr != sentinel.string.data() + sentinel.string.size())
            compiler_error(sentinel.pstring, "Invalid sentinel character.");

        int const converted = convert(utf32);
        if(converted < 0)
            compiler_error(sentinel.pstring, "Sentinel character must appear in charmap.");

        m_sentinel = converted;
    }
}

int charmap_t::convert(char32_t ch) const
{
    if(auto* result = m_map.lookup(ch))
        return result->second;
    return -1;
}

void charmap_t::set_group_data()
{
    assert(compiler_phase() == PHASE_CHARMAP_GROUPS);
    assert(!m_group_data);

    if(mods() && (mods()->explicit_lists & MODL_STOWS))
    {
        mods()->for_each_list_data(MODL_STOWS, [&](group_data_ht gd, pstring_t pstring)
        {
            if(m_group_data)
                compiler_error(pstring, "Too many 'stores' mods. Expecting one or zero.");

            m_group_data = gd;
        });

        if(!m_group_data)
            compiler_error(global.pstring(), "Invalid 'stores' mod.");
    }
}

void charmap_t::set_all_group_data()
{
    assert(compiler_phase() == PHASE_CHARMAP_GROUPS);

    for(charmap_t& charmap : charmap_ht::values())
        charmap.set_group_data();
}

////////////////////
// free functions //
////////////////////

std::string to_string(global_class_t gclass)
{
    switch(gclass)
    {
    default: return "bad global class";
#define X(x) case x: return #x;
    GLOBAL_CLASS_XENUM
#undef X
    }
}

fn_t const& get_main_mode()
{
    assert(compiler_phase() > PHASE_PARSE);

    using namespace std::literals;
    global_t const* main_mode = global_t::lookup_sourceless("main"sv);

    if(!main_mode || main_mode->gclass() != GLOBAL_FN || main_mode->impl<fn_t>().fclass != FN_MODE)
        throw compiler_error_t(fmt_error("Missing definition of mode main. Program has no entry point."));

    fn_t const& main_fn = main_mode->impl<fn_t>();
    if(main_fn.def().num_params > 0)
        compiler_error(main_fn.def().local_vars[0].decl.name, "Mode main cannot have parameters.");

    return main_fn;
}

void add_idep(ideps_map_t& map, global_t* global, idep_pair_t pair)
{
    auto result = map.emplace(global, pair);
    if(!result.second)
    {
        idep_pair_t& prev = result.first.underlying->second;
        prev.calc = std::min(prev.calc, pair.calc);
        prev.depends_on = std::max(prev.depends_on, pair.depends_on);
    }
}

charmap_t const& get_charmap(pstring_t from, global_t const& global)
{
    if(global.gclass() != GLOBAL_CHARMAP)
        compiler_error(from, fmt("% is not a charmap.", global.name));
   return global.impl<charmap_t>();
}
