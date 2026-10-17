#include "PenTouchInjector.hpp"

#include <algorithm>
#include <cstdio>
#include <windows.h>

namespace
{
    LONG ClampToRange(LONG value, LONG minimum, LONG maximum)
    {
        if (value < minimum)
        {
            return minimum;
        }
        if (value > maximum)
        {
            return maximum;
        }
        return value;
    }

    int16_t ClampTilt(int degrees)
    {
        if (degrees > 90)
        {
            return 90;
        }
        if (degrees < -90)
        {
            return -90;
        }
        return static_cast<int16_t>(degrees);
    }
}

PenTouchInjector::~PenTouchInjector()
{
    Stop();
}

bool PenTouchInjector::Start(const RECT& monitorRect)
{
    m_bounds = monitorRect;

    m_penDevice = ::CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    m_touchDevice = ::CreateSyntheticPointerDevice(PT_TOUCH, LTL_MAX_INPUT_POINTERS, POINTER_FEEDBACK_DEFAULT);
    if (m_penDevice == nullptr || m_touchDevice == nullptr)
    {
        std::fprintf(stderr, "[inject] CreateSyntheticPointerDevice failed err=%lu\n", ::GetLastError());
        return false;
    }
    std::fprintf(stderr, "[inject] synthetic pen and 10-pointer touch devices active\n");
    return true;
}

void PenTouchInjector::Stop()
{
#ifdef __MINGW32__
    if (m_penDevice != nullptr)
    {
        ::DestroySyntheticPointerDevice(static_cast<HSYNTHETICPOINTERDEVICE>(m_penDevice));
    }
    if (m_touchDevice != nullptr)
    {
        ::DestroySyntheticPointerDevice(static_cast<HSYNTHETICPOINTERDEVICE>(m_touchDevice));
    }
#endif
    m_penDevice = nullptr;
    m_touchDevice = nullptr;
    m_penState.clear();
    m_touchState.clear();
    m_penKnown.clear();
    m_touchKnown.clear();
}

void PenTouchInjector::BuildPointerEntry(bool pen, bool first, const Entry& entry, POINTER_TYPE_INFO& info) const
{
    const LtlPointerPayload& payload = entry.payload;

    const LONG spanX = m_bounds.right - m_bounds.left;
    const LONG spanY = m_bounds.bottom - m_bounds.top;
    const LONG pixelX = ClampToRange(m_bounds.left + static_cast<LONG>(payload.x * static_cast<float>(spanX)), m_bounds.left, m_bounds.right - 1);
    const LONG pixelY = ClampToRange(m_bounds.top + static_cast<LONG>(payload.y * static_cast<float>(spanY)), m_bounds.top, m_bounds.bottom - 1);

    UINT32 deviceState = 0;
    switch (payload.pointerState)
    {
    case LtlPointerHovering:
        deviceState = POINTER_FLAG_INRANGE;
        break;
    case LtlPointerContact:
        deviceState = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        break;
    default:
        deviceState = 0;
        break;
    }
    if (entry.released)
    {
        deviceState = POINTER_FLAG_UP;
    }
    if (entry.fresh)
    {
        deviceState |= POINTER_FLAG_NEW;
    }
    if (first)
    {
        deviceState |= POINTER_FLAG_PRIMARY;
    }

    UINT32 buttonFlags = 0;
    if (payload.buttons == LtlButtonStylusPrimary)
    {
        buttonFlags = POINTER_FLAG_FIRSTBUTTON;
    }
    else if (payload.buttons == LtlButtonStylusSecondary)
    {
        buttonFlags = POINTER_FLAG_SECONDBUTTON;
    }

    const UINT32 pressure = static_cast<UINT32>(ClampToRange(static_cast<LONG>(payload.pressure * 1024.0f), 0, 1024));

    ::ZeroMemory(&info, sizeof(info));

    info.type = static_cast<POINTER_INPUT_TYPE>(pen ? PT_PEN : PT_TOUCH);
    info.penInfo.pointerInfo.pointerType = static_cast<POINTER_INPUT_TYPE>(pen ? PT_PEN : PT_TOUCH);
    info.penInfo.pointerInfo.pointerId = payload.pointerId;
    info.penInfo.pointerInfo.pointerFlags = deviceState | buttonFlags;
    info.penInfo.pointerInfo.ptPixelLocation.x = pixelX;
    info.penInfo.pointerInfo.ptPixelLocation.y = pixelY;

    if (pen)
    {
        UINT32 penFlags = 0;
        if (payload.buttons == LtlButtonStylusPrimary || payload.buttons == LtlButtonStylusSecondary)
        {
            penFlags |= PEN_FLAG_BARREL;
        }
        if (payload.tool == LtlToolEraser)
        {
            penFlags |= PEN_FLAG_INVERTED;
        }

        info.penInfo.penFlags = penFlags;
        info.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
        info.penInfo.pressure = payload.pointerState == LtlPointerContact ? pressure : 0;
        info.penInfo.rotation = 0;
        info.penInfo.tiltX = ClampTilt(payload.tiltXDegrees);
        info.penInfo.tiltY = ClampTilt(payload.tiltYDegrees);
    }
    else
    {
        info.touchInfo.touchFlags = TOUCH_FLAG_NONE;
        info.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        info.touchInfo.rcContact.left = ClampToRange(pixelX - 2, m_bounds.left, m_bounds.right);
        info.touchInfo.rcContact.top = ClampToRange(pixelY - 2, m_bounds.top, m_bounds.bottom);
        info.touchInfo.rcContact.right = ClampToRange(pixelX + 2, m_bounds.left, m_bounds.right);
        info.touchInfo.rcContact.bottom = ClampToRange(pixelY + 2, m_bounds.top, m_bounds.bottom);
        info.touchInfo.pressure = payload.pointerState == LtlPointerContact ? pressure : 0;
        info.touchInfo.orientation = 0;
    }
}

void PenTouchInjector::FlushFrame(bool pen, const Entry* entries, uint32_t count)
{
#ifdef __MINGW32__
    const HSYNTHETICPOINTERDEVICE device = pen
        ? static_cast<HSYNTHETICPOINTERDEVICE>(m_penDevice)
        : static_cast<HSYNTHETICPOINTERDEVICE>(m_touchDevice);
#else
    const HPOINTER device = pen ? m_penDevice : m_touchDevice;
#endif
    uint32_t effective = count;
    if (pen && effective > 1)
    {
        effective = 1;
    }
    if (device == nullptr || effective == 0)
    {
        return;
    }

    m_frame.assign(effective, POINTER_TYPE_INFO{});
    for (uint32_t i = 0; i < effective; ++i)
    {
        BuildPointerEntry(pen, i == 0, entries[i], m_frame[i]);
    }
    if (!::InjectSyntheticPointerInput(device, m_frame.data(), effective))
    {
        std::fprintf(stderr, "[inject] InjectSyntheticPointerInput failed err=%lu\n", ::GetLastError());
    }
}

void PenTouchInjector::Inject(const LtlInputEventPayload& event)
{
    if (m_penDevice == nullptr || m_touchDevice == nullptr)
    {
        return;
    }

    const bool pen = event.deviceKind == LtlInputDevicePen;
    auto& state = pen ? m_penState : m_touchState;
    auto& known = pen ? m_penKnown : m_touchKnown;

    m_entries.clear();

    const uint32_t count = event.pointerCount > LTL_MAX_INPUT_POINTERS ? LTL_MAX_INPUT_POINTERS : event.pointerCount;
    if (event.action == LtlActionCancel)
    {
        for (const auto& [pointerId, payload] : state)
        {
            m_entries.push_back(Entry{ payload, true, false });
            known.erase(pointerId);
        }
        state.clear();
        FlushFrame(pen, m_entries.data(), static_cast<uint32_t>(m_entries.size()));
        return;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const LtlPointerPayload& update = event.pointers[i];
        if (update.pointerState == LtlPointerIdle)
        {
            const auto it = state.find(update.pointerId);
            if (it != state.end())
            {
                m_entries.push_back(Entry{ it->second, true, false });
                state.erase(it);
                known.erase(update.pointerId);
            }
            continue;
        }

        const bool fresh = !known.contains(update.pointerId);
        known.insert(update.pointerId);
        state[update.pointerId] = update;
        m_entries.push_back(Entry{ update, false, fresh });
    }

    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& left, const Entry& right)
    {
        return left.payload.pointerId < right.payload.pointerId;
    });
    if (!m_entries.empty())
    {
        FlushFrame(pen, m_entries.data(), static_cast<uint32_t>(m_entries.size()));
    }
}
