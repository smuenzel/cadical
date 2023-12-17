#include "internal.hpp"
#include "options.hpp"

namespace CaDiCaL {

// Failed literal handling as pioneered by MiniSAT.  This first function
// adds an assumption literal onto the assumption stack.

void Internal::assume (int lit) {
  if (level && !opts.ilbassumptions)
    backtrack ();
  else if (val (lit) < 0)
    backtrack (max (0, var (lit).level - 1));
  Flags &f = flags (lit);
  const unsigned char bit = bign (lit);
  if (f.assumed & bit) {
    LOG ("ignoring already assumed %d", lit);
    return;
  }
  LOG ("assume %d", lit);
  f.assumed |= bit;
  assumptions.push_back (lit);
  freeze (lit);
}

// for LRAT we actually need to implement recursive dfs
// I don't know how to do this non-recursively...
// for non-lrat use bfs
//
void Internal::assume_analyze_literal (int lit) {
  assert (lit);
  Flags &f = flags (lit);
  if (f.seen)
    return;
  f.seen = true;
  analyzed.push_back (lit);
  Var &v = var (lit);
  assert (val (lit) < 0);
  if (v.reason == external_reason) {
    v.reason = wrapped_learn_external_reason_clause (-lit);
    assert (v.reason || !v.level);
    if (!v.reason) {
      if (opts.reimply) {
        trail.push_back (-lit);
        v.trail = trail.size ();
      }
    }
  }
  assert (v.reason != external_reason);
  if (!v.level) {
    const unsigned uidx = vlit (-lit);
    uint64_t id = unit_clauses[uidx];
    assert (id);
    lrat_chain.push_back (id);
    return;
  }
  if (v.reason) {
    assert (v.level);
    LOG (v.reason, "analyze reason");
    for (const auto &other : *v.reason) {
      assume_analyze_literal (other);
    }
    lrat_chain.push_back (v.reason->id);
    return;
  }
  assert (assumed (-lit));
  LOG ("failed assumption %d", -lit);
  clause.push_back (lit);
}

void Internal::assume_analyze_reason (int lit, Clause *reason) {
  assert (reason);
  assert (lrat_chain.empty ());
  assert (reason != external_reason);
  assert (lrat);
  for (const auto &other : *reason)
    if (other != lit)
      assume_analyze_literal (other);
  lrat_chain.push_back (reason->id);
}

// Find all failing assumptions starting from the one on the assumption
// stack with the lowest decision level.  This goes back to MiniSAT and is
// called 'analyze_final' there.

void Internal::failing () {

  START (analyze);

  LOG ("analyzing failing assumptions");

  assert (analyzed.empty ());
  assert (clause.empty ());
  assert (lrat_chain.empty ());
  assert (!marked_failed);
  assert (!conflict_id);

  if (!unsat_constraint) {
    // Search for failing assumptions in the (internal) assumption stack.

    // There are in essence three cases: (1) An assumption is falsified on
    // the root-level and then 'failed_unit' is set to that assumption, (2)
    // two clashing assumptions are assumed and then 'failed_clashing' is
    // set to the second assumed one, or otherwise (3) there is a failing
    // assumption 'first_failed' with minimum (non-zero) decision level
    // 'failed_level'.

    int failed_unit = 0;
    int failed_clashing = 0;
    int first_failed = 0;
    int failed_level = INT_MAX;
    int efailed = 0;

    for (auto &elit : external->assumptions) {
      int lit = external->e2i[abs (elit)];
      if (elit < 0)
        lit = -lit;
      if (val (lit) >= 0)
        continue;
      const Var &v = var (lit);
      if (!v.level) {
        failed_unit = lit;
        efailed = elit;
        break;
      }
      if (failed_clashing)
        continue;
      if (v.reason == external_reason) {
        Var &ev = var (lit);
        ev.reason = learn_external_reason_clause (-lit);
        if (!ev.reason) {
          ev.level = 0;
          failed_unit = lit;
          efailed = elit;
          break;
        }
        ev.level = 0;
        // Recalculate assignment level
        for (const auto &other : *ev.reason) {
          if (other == -lit)
            continue;
          assert (val (other));
          int tmp = var (other).level;
          if (tmp > ev.level)
            ev.level = tmp;
        }
        if (!ev.level) {
          failed_unit = lit;
          efailed = elit;
          break;
        }
      }
      assert (v.reason != external_reason);
      if (!v.reason) {
        failed_clashing = lit;
        efailed = elit;
      } else if (!first_failed || v.level < failed_level) {
        first_failed = lit;
        efailed = elit;
        failed_level = v.level;
      }
    }

    assert (clause.empty ());

    // Get the 'failed' assumption from one of the three cases.
    int failed;
    if (failed_unit)
      failed = failed_unit;
    else if (failed_clashing)
      failed = failed_clashing;
    else
      failed = first_failed;
    assert (failed);
    assert (efailed);

    // In any case mark literal 'failed' as failed assumption.
    {
      Flags &f = flags (failed);
      const unsigned bit = bign (failed);
      assert (!(f.failed & bit));
      f.failed |= bit;
    }

    // First case (1).
    if (failed_unit) {
      assert (failed == failed_unit);
      LOG ("root-level falsified assumption %d", failed);
      if (proof) {
        if (lrat) {
          unsigned eidx = (efailed > 0) + 2u * (unsigned) abs (efailed);
          assert ((size_t) eidx < external->ext_units.size ());
          const uint64_t id = external->ext_units[eidx];
          if (id) {
            lrat_chain.push_back (id);
          } else {
            const unsigned uidx = vlit (-failed_unit);
            uint64_t id = unit_clauses[uidx];
            assert (id);
            lrat_chain.push_back (id);
          }
        }
        proof->add_assumption_clause (++clause_id, -efailed, lrat_chain);
        conclusion.push_back (clause_id);
        lrat_chain.clear ();
      }
      goto DONE;
    }

    // Second case (2).
    if (failed_clashing) {
      assert (failed == failed_clashing);
      LOG ("clashing assumptions %d and %d", failed, -failed);
      Flags &f = flags (-failed);
      const unsigned bit = bign (-failed);
      assert (!(f.failed & bit));
      f.failed |= bit;
      if (proof) {
        vector<int> clash = {externalize (failed), externalize (-failed)};
        proof->add_assumption_clause (++clause_id, clash, lrat_chain);
        conclusion.push_back (clause_id);
      }
      goto DONE;
    }

    // Fall through to third case (3).
    LOG ("starting with assumption %d falsified on minimum decision level "
         "%d",
         first_failed, failed_level);

    assert (first_failed);
    assert (failed_level > 0);

    // The 'analyzed' stack serves as working stack for a BFS through the
    // implication graph until decisions, which are all assumptions, or
    // units are reached.  This is simpler than corresponding code in
    // 'analyze'.
    {
      LOG ("failed assumption %d", first_failed);
      Flags &f = flags (first_failed);
      assert (!f.seen);
      f.seen = true;
      assert (f.failed & bign (first_failed));
      analyzed.push_back (-first_failed);
      clause.push_back (-first_failed);
    }
  } else {
    // unsat_constraint
    // The assumptions necessary to fail each literal in the constraint are
    // collected.
    for (auto lit : constraint) {
      lit *= -1;
      assert (lit != INT_MIN);
      flags (lit).seen = true;
      analyzed.push_back (lit);
    }
  }

  {
    // used for unsat_constraint lrat
    vector<vector<uint64_t>> constraint_chains;
    vector<vector<int>> constraint_clauses;
    vector<int> sum_constraints;
    vector<int> econstraints;
    for (auto &elit : external->constraint) {
      int lit = external->e2i[abs (elit)];
      if (elit < 0)
        lit = -lit;
      if (!lit)
        continue;
      Flags &f = flags (lit);
      if (f.seen)
        continue;
      if (std::find (econstraints.begin (), econstraints.end (), elit) !=
          econstraints.end ())
        continue;
      econstraints.push_back (elit);
    }

    // no LRAT do bfs as it was before
    if (!lrat) {
      size_t next = 0;
      while (next < analyzed.size ()) {
        const int lit = analyzed[next++];
        assert (val (lit) > 0);
        Var &v = var (lit);
        if (!v.level)
          continue;
        if (v.reason == external_reason) {
          v.reason = wrapped_learn_external_reason_clause (lit);
          if (!v.reason) {
            v.level = 0;
            if (opts.reimply) {
              trail.push_back (lit);
              v.trail = trail.size ();
            }
            continue;
          }
        }
        assert (v.reason != external_reason);
        if (v.reason) {
          assert (v.level);
          LOG (v.reason, "analyze reason");
          for (const auto &other : *v.reason) {
            Flags &f = flags (other);
            if (f.seen)
              continue;
            f.seen = true;
            assert (val (other) < 0);
            analyzed.push_back (-other);
          }
        } else {
          assert (assumed (lit));
          LOG ("failed assumption %d", lit);
          clause.push_back (-lit);
          Flags &f = flags (lit);
          const unsigned bit = bign (lit);
          assert (!(f.failed & bit));
          f.failed |= bit;
        }
      }
      clear_analyzed_literals ();
    } else if (!unsat_constraint) { // LRAT for case (3)
      assert (clause.size () == 1);
      const int lit = clause[0];
      Var &v = var (lit);
      assert (v.reason);
      if (v.reason == external_reason) { // does this even happen?
        v.reason = wrapped_learn_external_reason_clause (lit);
      }
      assert (v.reason != external_reason);
      if (v.reason)
        assume_analyze_reason (lit, v.reason);
      else {
        const unsigned uidx = vlit (lit);
        uint64_t id = unit_clauses[uidx];
        assert (id);
        lrat_chain.push_back (id);
      }
      for (auto &lit : clause) {
        Flags &f = flags (lit);
        const unsigned bit = bign (-lit);
        if (!(f.failed & bit))
          f.failed |= bit;
      }
      clear_analyzed_literals ();
    } else { // LRAT for unsat_constraint
      assert (clause.empty ());
      clear_analyzed_literals ();
      for (auto lit : constraint) {
        // make sure nothing gets marked failed twice
        // also might shortcut the case where
        // lrat_chain is empty because clause is tautological
        assert (lit != INT_MIN);
        assume_analyze_literal (lit);
        vector<uint64_t> empty;
        vector<int> empty2;
        constraint_chains.push_back (empty);
        constraint_clauses.push_back (empty2);
        for (auto ign : clause) {
          constraint_clauses.back ().push_back (ign);
          Flags &f = flags (ign);
          const unsigned bit = bign (-ign);
          if (!(f.failed & bit)) {
            sum_constraints.push_back (ign);
            assert (!(f.failed & bit));
            f.failed |= bit;
          }
        }
        clause.clear ();
        clear_analyzed_literals ();
        for (auto p : lrat_chain) {
          constraint_chains.back ().push_back (p);
        }
        lrat_chain.clear ();
      }
      for (auto &lit : sum_constraints)
        clause.push_back (lit);
    }
    clear_analyzed_literals ();

    // TODO: We can not do clause minimization here, right?

    VERBOSE (1, "found %zd failed assumptions %.0f%%", clause.size (),
             percent (clause.size (), assumptions.size ()));

    // We do not actually need to learn this clause, since the conflict is
    // forced already by some other clauses.  There is also no bumping
    // of variables nor clauses necessary.  But we still want to check
    // correctness of the claim that the determined subset of failing
    // assumptions are a high-level core or equivalently their negations
    // form a unit-implied clause.
    //
    if (!unsat_constraint) {
      external->check_learned_clause ();
      if (proof) {
        vector<int> eclause;
        for (auto &lit : clause)
          eclause.push_back (externalize (lit));
        proof->add_assumption_clause (++clause_id, eclause, lrat_chain);
        conclusion.push_back (clause_id);
      }
    } else {
      assert (!lrat || (constraint.size () == constraint_clauses.size () &&
                        constraint.size () == constraint_chains.size ()));
      for (auto p = constraint.rbegin (); p != constraint.rend (); p++) {
        const auto &lit = *p;
        if (lrat) {
          clause.clear ();
          for (auto &ign : constraint_clauses.back ())
            clause.push_back (ign);
          constraint_clauses.pop_back ();
        }
        clause.push_back (-lit);
        external->check_learned_clause ();
        if (proof) {
          if (lrat) {
            for (auto p : constraint_chains.back ()) {
              lrat_chain.push_back (p);
            }
            constraint_chains.pop_back ();
            LOG (lrat_chain, "assume proof chain with constraints");
          }
          vector<int> eclause;
          for (auto &lit : clause)
            eclause.push_back (externalize (lit));
          proof->add_assumption_clause (++clause_id, eclause, lrat_chain);
          conclusion.push_back (clause_id);
          lrat_chain.clear ();
        }
        clause.pop_back ();
      }
      if (proof) {
        for (auto &elit : econstraints) {
          if (lrat) {
            unsigned eidx = (elit > 0) + 2u * (unsigned) abs (elit);
            assert ((size_t) eidx < external->ext_units.size ());
            const uint64_t id = external->ext_units[eidx];
            if (id) {
              lrat_chain.push_back (id);
            } else {
              int lit = external->e2i[abs (elit)];
              if (elit < 0)
                lit = -lit;
              const unsigned uidx = vlit (-lit);
              uint64_t id = unit_clauses[uidx];
              assert (id);
              lrat_chain.push_back (id);
            }
          }
          proof->add_assumption_clause (++clause_id, -elit, lrat_chain);
          conclusion.push_back (clause_id);
          lrat_chain.clear ();
        }
      }
    }
    lrat_chain.clear ();
    clause.clear ();
  }

DONE:

  STOP (analyze);
}

bool Internal::failed (int lit) {
  if (!marked_failed) {
    if (!conflict_id)
      failing ();
    marked_failed = true;
  }
  conclude_unsat ();
  Flags &f = flags (lit);
  const unsigned bit = bign (lit);
  return (f.failed & bit) != 0;
}

void Internal::conclude_unsat () {
  if (!proof || concluded)
    return;
  concluded = true;
  if (!marked_failed) {
    assert (conclusion.empty ());
    if (!conflict_id)
      failing ();
    marked_failed = true;
  }
  ConclusionType con;
  if (conflict_id)
    con = CONFLICT;
  else if (unsat_constraint)
    con = CONSTRAINT;
  else
    con = ASSUMPTIONS;
  proof->conclude_unsat (con, conclusion);
}

void Internal::reset_concluded () {
  if (proof)
    proof->reset_assumptions ();
  if (concluded) {
    LOG ("reset concluded");
    concluded = false;
  }
  if (conflict_id) {
    assert (conclusion.size () == 1);
    return;
  }
  conclusion.clear ();
}

// Add the start of each incremental phase (leaving the state
// 'UNSATISFIABLE' actually) we reset all assumptions.

void Internal::reset_assumptions () {
  for (const auto &lit : assumptions) {
    Flags &f = flags (lit);
    const unsigned char bit = bign (lit);
    f.assumed &= ~bit;
    f.failed &= ~bit;
    melt (lit);
  }
  LOG ("cleared %zd assumptions", assumptions.size ());
  assumptions.clear ();
  marked_failed = true;
}

// sort the assumptions by the current position on the trail and backtrack
// to the first place where the assumptions and the current trail differ.
void Internal::sort_and_reuse_assumptions () {
  assert (opts.ilbassumptions);
  if (assumptions.empty ())
    return;
  // set assumptions first, then sorted by position on the trail
  // unset literals are sorted by literal value
  std::sort (begin (assumptions), end (assumptions),
             [this] (int litA, int litB) {
               if (!val (litA) && val (litB))
                 return false;
               if (val (litA) && !val (litB))
                 return true;
               if (!val (litA) && !val (litB))
                 return litA < litB;
               assert (val (litA) && val (litB));
               LOG ("%d -> %zd", litA,
                    ((uint64_t) var (litA).level << 32) +
                        (uint64_t) var (litA).trail);
               return ((uint64_t) var (litA).level << 32) +
                          (uint64_t) var (litA).trail <
                      ((uint64_t) var (litB).level << 32) +
                          (uint64_t) var (litB).trail;
             });
  int max_level = 0;
  for (auto lit : assumptions) {
    if (val (lit))
      max_level = var (lit).level;
    else
      break;
  }

  const int size = min (level + 1, max_level + 1);
  assert ((size_t) level == control.size () - 1);
  LOG (assumptions, "sorted assumptions");
  int target = 0;
  for (int i = 1, j = 0; i < size;) {
    const Level &l = control[i];
    const int lit = l.decision;
    const int alit = assumptions[j];
    const int lev = i;
    target = lev - 1;
    if (val (alit) &&
        var (alit).level < lev) { // we can ignore propagated assumptions
      ++j;
      continue;
    }
    ++i, ++j;
    if (!lit || var (lit).level !=
                    lev) { // removed literals or pseudo decision level
      if (val (alit) > 0 && var (alit).level < lev)
        continue;
      break;
    }
    if (l.decision == alit) {
      continue;
    }
    break;
  }

  if (target < level)
    backtrack (target);
  LOG ("assumptions allow for reuse of trail up to level %d", level);
  if ((size_t) level > assumptions.size ())
    stats.assumptionsreused += assumptions.size ();
  else
    stats.assumptionsreused += level;
}
} // namespace CaDiCaL
