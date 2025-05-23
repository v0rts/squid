/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "acl/BoolOps.h"
#include "acl/Checklist.h"
#include "debug/Stream.h"
#include "sbuf/SBuf.h"

/* Acl::NotNode */

Acl::NotNode::NotNode(Acl::Node *acl)
{
    assert(acl);
    name.reserveCapacity(1 + acl->name.length());
    name.append('!');
    name.append(acl->name);
    add(acl);
}

void
Acl::NotNode::parse()
{
    // Not implemented: by the time an upper level parser discovers
    // an '!' operator, there is nothing left for us to parse.
    assert(false);
}

int
Acl::NotNode::doMatch(ACLChecklist *checklist, Nodes::const_iterator start) const
{
    assert(start == nodes.begin()); // we only have one node

    if (checklist->matchChild(this, start))
        return 0; // converting match into mismatch

    if (!checklist->keepMatching())
        return -1; // suspend on async calls and stop on failures

    return 1; // converting mismatch into match
}

char const *
Acl::NotNode::typeString() const
{
    return "!";
}

SBufList
Acl::NotNode::dump() const
{
    SBufList text;
    text.push_back(name);
    return text;
}

/* Acl::AndNode */

char const *
Acl::AndNode::typeString() const
{
    return "and";
}

int
Acl::AndNode::doMatch(ACLChecklist *checklist, Nodes::const_iterator start) const
{
    // find the first node that does not match
    for (Nodes::const_iterator i = start; i != nodes.end(); ++i) {
        if (!checklist->matchChild(this, i))
            return checklist->keepMatching() ? 0 : -1;
    }

    // one and not zero on empty because in math empty product equals identity
    return 1; // no mismatches found (i.e., all kids matched)
}

void
Acl::AndNode::parse()
{
    // Not implemented: AndNode cannot be configured directly. See Acl::AllOf.
    assert(false);
}

/* Acl::OrNode */

char const *
Acl::OrNode::typeString() const
{
    return "any-of";
}

bool
Acl::OrNode::bannedAction(ACLChecklist *, Nodes::const_iterator) const
{
    return false;
}

int
Acl::OrNode::doMatch(ACLChecklist *checklist, Nodes::const_iterator start) const
{
    lastMatch_ = nodes.end();

    // find the first node that matches, but stop if things go wrong
    for (Nodes::const_iterator i = start; i != nodes.end(); ++i) {
        if (bannedAction(checklist, i))
            continue;
        if (checklist->matchChild(this, i)) {
            lastMatch_ = i;
            return 1;
        }

        if (!checklist->keepMatching())
            return -1; // suspend on async calls and stop on failures
    }

    // zero and not one on empty because in math empty sum equals zero
    return 0; // all nodes mismatched
}

void
Acl::OrNode::parse()
{
    // Not implemented: OrNode cannot be configured directly. See Acl::AnyOf.
    assert(false);
}

