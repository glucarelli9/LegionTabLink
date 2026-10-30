#pragma once

#include "packet_spec.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

class PenTouchInjector
{
public:
    PenTouchInjector() = default;
    ~PenTouchInjector();

    PenTouchInjector(const PenTouchInjector&) = delete;
    PenTouchInjector& operator=(const PenTouchInjector&) = delete;

    bool Start(const RECT& monitorRect);
    void Stop();
    void Inject(const LtlInputEventPayload& event);
    void UpdateBounds(const RECT& newBounds) { m_bounds = newBounds; }

private:
    struct Entry
    {
        LtlPointerPayload payload{};
        bool released = false;
        bool fresh = false;
    };

    void FlushFrame(bool pen, const Entry* entries, uint32_t count);
    void BuildPointerEntry(bool pen, bool first, const Entry& entry, POINTER_TYPE_INFO& info) const;

    void* m_penDevice = nullptr;
    void* m_touchDevice = nullptr;
    RECT m_bounds{};
    std::unordered_map<uint8_t, LtlPointerPayload> m_penState;
    std::unordered_map<uint8_t, LtlPointerPayload> m_touchState;
    std::unordered_set<uint8_t> m_penKnown;
    std::unordered_set<uint8_t> m_touchKnown;
    std::vector<Entry> m_entries;
    std::vector<POINTER_TYPE_INFO> m_frame;
};
