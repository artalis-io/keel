/*
 * event_turn.h: dispatch fairness for a backend that picks which of its ready registrations a wait
 * reports (a wait returns at most `max` events; the poll, Windows select() and pollcomp-watch scans).
 *
 * The scheme is LAPS. Each registration carries the lap in which it was last reported. A wait
 * reports only registrations not yet reported in the current lap; when it runs out of those before
 * `max`, the lap ends and it goes on with the next lap, skipping what it already reported in this
 * wait. A new registration joins as already reported in the current lap, so arrivals wait for the
 * next lap instead of extending this one. Order in the backend's array plays no part, so a removal
 * may move registrations freely (swap with the last one).
 *
 * The bound this gives, for registrations that stay ready: a lap reports each registration present
 * at its start once and ends within ceil(N / max) waits (it never takes on new members, and every
 * wait but its last reports `max` of them), so a registration is reported within the rest of the
 * current lap plus the next one: 2 * ceil(N / max) waits, N the most registrations over that
 * period; ceil(N / max) for a set registered together. No registration is reported twice while
 * another one, present and ready throughout, has not been reported once. Connection churn (every
 * reported registration removed and a new one added each wait) does not change either bound.
 *
 * Header-only; each backend TU keeps its own per-registration array of KlEventTurn.
 */
#ifndef KEEL_EVENT_TURN_H
#define KEEL_EVENT_TURN_H

typedef struct {
    unsigned lap;   /* the lap this registration was last reported in (or joined in) */
    unsigned seq;   /* the wait it was last reported by */
} KlEventTurn;

typedef struct {
    unsigned lap;   /* the current lap */
    unsigned seq;   /* the current wait */
} KlEventTurns;

/* A new registration: counts as reported in the current lap (it is first reported in the next). */
static inline void kl_turn_join(const KlEventTurns *t, KlEventTurn *r) {
    r->lap = t->lap;
    r->seq = t->seq;   /* != the next wait's seq */
}

/* Start a wait. */
static inline void kl_turns_begin(KlEventTurns *t) { t->seq++; }

/* The lap is over (no due registration was ready): start the next one. */
static inline void kl_turns_next_lap(KlEventTurns *t) { t->lap++; }

/* May this registration be reported now: not yet in this lap, and not already by this wait. */
static inline int kl_turn_due(const KlEventTurns *t, const KlEventTurn *r) {
    return r->lap != t->lap && r->seq != t->seq;
}

/* It is being reported. */
static inline void kl_turn_take(const KlEventTurns *t, KlEventTurn *r) {
    r->lap = t->lap;
    r->seq = t->seq;
}

#endif
