/* SPDX-License-Identifier: BSD-3-Clause */
#include "PujjoIpc4Protocol.h"

static PJ_DWORD read_le32(const PJ_BYTE *p)
{
    return (PJ_DWORD)p[0] | ((PJ_DWORD)p[1] << 8) |
           ((PJ_DWORD)p[2] << 16) | ((PJ_DWORD)p[3] << 24);
}
static void write_le32(PJ_BYTE *p, PJ_DWORD v)
{
    p[0]=(PJ_BYTE)(v & 255u);
    p[1]=(PJ_BYTE)((v >> 8) & 255u);
    p[2]=(PJ_BYTE)((v >> 16) & 255u);
    p[3]=(PJ_BYTE)((v >> 24) & 255u);
}
int pj_ipc4_is_reply(PJ_DWORD primary)
{
    return (primary & PJ_IPC4_DIRECTION_MASK) != 0u;
}
int pj_ipc4_is_notification(PJ_DWORD primary)
{
    return (primary & (PJ_IPC4_DIRECTION_MASK | PJ_IPC4_TARGET_MASK |
                        PJ_IPC4_TYPE_MASK | PJ_IPC4_RESERVED_MASK)) ==
           (PJ_IPC4_GLOBAL_NOTIFICATION << 24);
}
int pj_ipc4_is_fw_ready(PJ_DWORD primary)
{
    return pj_ipc4_is_notification(primary) &&
           ((primary >> 16) & 255u) == PJ_IPC4_NOTIFY_FW_READY;
}
int pj_ipc4_reply_matches(PJ_DWORD request, PJ_DWORD reply)
{
    if ((request | reply) & PJ_IPC4_RESERVED_MASK) return 0;
    if (pj_ipc4_is_reply(request) || !pj_ipc4_is_reply(reply)) return 0;
    /* Type 27 is reserved for asynchronous notifications, not command replies.
     * Reject it even if a corrupted header sets the reply direction bit. */
    if ((request & (PJ_IPC4_TYPE_MASK | PJ_IPC4_TARGET_MASK)) ==
         (PJ_IPC4_GLOBAL_NOTIFICATION << 24) ||
        (reply & (PJ_IPC4_TYPE_MASK | PJ_IPC4_TARGET_MASK)) ==
         (PJ_IPC4_GLOBAL_NOTIFICATION << 24)) return 0;
    /* IPC4's type/target fields are stable across the common response path.
     * Firmware-specific lower header fields are NOT a universally unique ID. */
    return (request & PJ_IPC4_CORRELATION_MASK) ==
           (reply & PJ_IPC4_CORRELATION_MASK);
}
pj_ipc4_status pj_ipc4_validate(const pj_ipc4_packet *packet)
{
    if (!packet) return PJ_IPC4_BAD_PARAMETER;
    if (packet->primary & PJ_IPC4_RESERVED_MASK) return PJ_IPC4_INVALID_HEADER;
    if (packet->payload_bytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD)
        return PJ_IPC4_TOO_LARGE;
    if (packet->payload_bytes != 0u && !packet->payload)
        return PJ_IPC4_BAD_PARAMETER;
    return PJ_IPC4_VALID;
}
pj_ipc4_status pj_ipc4_decode(const PJ_BYTE *input, PJ_SIZE input_bytes,
                               pj_ipc4_packet *packet)
{
    pj_ipc4_packet p;
    pj_ipc4_status s;
    if (!input || !packet) return PJ_IPC4_BAD_PARAMETER;
    if (input_bytes < PJ_IPC4_HEADER_BYTES) return PJ_IPC4_TRUNCATED;
    if (input_bytes - PJ_IPC4_HEADER_BYTES > PJ_IPC4_MAX_MAILBOX_PAYLOAD)
        return PJ_IPC4_TOO_LARGE;
    p.primary=read_le32(input);
    p.extension=read_le32(input+4u);
    p.payload=input+PJ_IPC4_HEADER_BYTES;
    p.payload_bytes=input_bytes-PJ_IPC4_HEADER_BYTES;
    s=pj_ipc4_validate(&p);
    if (s != PJ_IPC4_VALID) return s;
    *packet=p; /* failed calls leave caller's packet unchanged */
    return PJ_IPC4_VALID;
}
pj_ipc4_status pj_ipc4_encode(const pj_ipc4_packet *packet,
                               PJ_BYTE *output, PJ_SIZE output_bytes,
                               PJ_SIZE *written)
{
    PJ_SIZE i;
    pj_ipc4_status s;
    if (!output || !written) return PJ_IPC4_BAD_PARAMETER;
    *written=0u;
    s=pj_ipc4_validate(packet);
    if (s != PJ_IPC4_VALID) return s;
    if (output_bytes < PJ_IPC4_HEADER_BYTES + packet->payload_bytes)
        return PJ_IPC4_BUFFER_SMALL;
    /* Caller must provide disjoint buffers: no in-place encoding. */
    write_le32(output,packet->primary);
    write_le32(output+4u,packet->extension);
    for (i=0u;i<packet->payload_bytes;++i)
        output[PJ_IPC4_HEADER_BYTES+i]=packet->payload[i];
    *written=PJ_IPC4_HEADER_BYTES+packet->payload_bytes;
    return PJ_IPC4_VALID;
}

int pj_ipc4_mmio_span_valid(PJ_DWORD mapped_bytes, PJ_DWORD offset, PJ_DWORD bytes)
{
    return bytes != 0u && (offset & 3u) == 0u && (bytes & 3u) == 0u &&
           offset <= mapped_bytes && bytes <= mapped_bytes - offset;
}

/* IPC4 graph command encoders, per include/sof/ipc4/header.h and the
 * Linux SOF IPC4 topology driver's message construction. No I/O. */
static pj_ipc4_status graph_packet(PJ_DWORD primary, PJ_DWORD extension,
    const PJ_BYTE *payload, PJ_SIZE bytes, pj_ipc4_packet *out)
{
    pj_ipc4_packet p;
    if (!out) return PJ_IPC4_BAD_PARAMETER;
    p.primary=primary;
    p.extension=extension;
    p.payload=payload;
    p.payload_bytes=bytes;
    *out=p;
    return PJ_IPC4_VALID;
}
pj_ipc4_status pj_ipc4_pipeline_create(PJ_DWORD id, PJ_DWORD priority,
    PJ_DWORD memory_pages, PJ_DWORD core, PJ_DWORD low_power, pj_ipc4_packet *out)
{
    if (id>255u || priority>31u || memory_pages>2047u ||
        core>15u || low_power>1u)
        return PJ_IPC4_BAD_PARAMETER;
    return graph_packet((17u<<24)|(id<<16)|(priority<<11)|memory_pages,
                        (core<<20)|low_power, 0, 0u, out);
}
pj_ipc4_status pj_ipc4_pipeline_delete(PJ_DWORD id, pj_ipc4_packet *out)
{
    if (id>255u) return PJ_IPC4_BAD_PARAMETER;
    return graph_packet((18u<<24)|(id<<16),0u,0,0u,out);
}
pj_ipc4_status pj_ipc4_pipeline_state(PJ_DWORD id, PJ_DWORD state, pj_ipc4_packet *out)
{
    /* RESET, PAUSED, RUNNING, EOS. Invalid/uninitialized are not commands. */
    if (id>255u || state<2u || state>5u) return PJ_IPC4_BAD_PARAMETER;
    return graph_packet((19u<<24)|(id<<16)|state,0u,0,0u,out);
}
pj_ipc4_status pj_ipc4_module_init(PJ_DWORD module, PJ_DWORD instance,
    PJ_DWORD pipeline, PJ_DWORD core, PJ_DWORD domain,
    const PJ_BYTE *payload, PJ_SIZE bytes, pj_ipc4_packet *out)
{
    if (!out || module>65535u || instance>255u || pipeline>255u ||
        core>15u || domain>1u || !payload || bytes==0u || (bytes&3u)!=0u)
        return PJ_IPC4_BAD_PARAMETER;
    if (bytes>PJ_IPC4_MAX_MAILBOX_PAYLOAD) return PJ_IPC4_TOO_LARGE;
    return graph_packet(PJ_IPC4_TARGET_MASK|(instance<<16)|module,
        (PJ_DWORD)(bytes/4u)|(pipeline<<16)|(core<<24)|(domain<<28),
        payload,bytes,out);
}
pj_ipc4_status pj_ipc4_module_link(PJ_DWORD bind, PJ_DWORD src_module,
    PJ_DWORD src_instance, PJ_DWORD src_queue, PJ_DWORD dst_module,
    PJ_DWORD dst_instance, PJ_DWORD dst_queue, pj_ipc4_packet *out)
{
    if (bind>1u || src_module>65535u || dst_module>65535u ||
        src_instance>255u || dst_instance>255u || src_queue>7u || dst_queue>7u)
        return PJ_IPC4_BAD_PARAMETER;
    return graph_packet(PJ_IPC4_TARGET_MASK|((bind?5u:6u)<<24)|
        (src_instance<<16)|src_module,
        (src_queue<<27)|(dst_queue<<24)|(dst_instance<<16)|dst_module,0,0u,out);
}

pj_ipc4_status pj_ipc4_large_config_set(PJ_DWORD module, PJ_DWORD instance,
    PJ_DWORD parameter, const PJ_BYTE *payload, PJ_SIZE bytes, pj_ipc4_packet *out)
{
    if (!out || module > 65535u || instance > 255u ||
        parameter > 255u || !payload || bytes == 0u)
        return PJ_IPC4_BAD_PARAMETER;
    if (bytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD) return PJ_IPC4_TOO_LARGE;
    return graph_packet(PJ_IPC4_TARGET_MASK | (4u << 24) |
                        (instance << 16) | module,
                        (3u << 28) | (parameter << 20) | (PJ_DWORD)bytes,
                        payload, bytes, out);
}

pj_ipc4_status pj_ipc4_large_config_get(PJ_DWORD module, PJ_DWORD instance,
    PJ_DWORD parameter, PJ_DWORD capacity, pj_ipc4_packet *out)
{
    if (!out || module > 65535u || instance > 255u || parameter > 255u || !capacity)
        return PJ_IPC4_BAD_PARAMETER;
    if (capacity > PJ_IPC4_MAX_MAILBOX_PAYLOAD) return PJ_IPC4_TOO_LARGE;
    return graph_packet(PJ_IPC4_MODULE_LARGE_GET | (instance << 16) | module,
                        (1u << 29) | (parameter << 20) | capacity, 0, 0u, out);
}

pj_ipc4_status pj_ipc4_large_reply_size(PJ_DWORD request_extension,
    PJ_DWORD reply_extension, PJ_DWORD capacity, PJ_DWORD *bytes)
{
    const PJ_DWORD parameter_mask = 0x0ff00000u;
    const PJ_DWORD size_mask = 0x000fffffu;
    const PJ_DWORD first_block = 1u << 29;
    const PJ_DWORD last_block = 1u << 28;
    PJ_DWORD size;

    if (!bytes || !capacity || capacity > PJ_IPC4_MAX_MAILBOX_PAYLOAD)
        return PJ_IPC4_BAD_PARAMETER;
    if ((request_extension & ~(parameter_mask | size_mask)) != first_block ||
        (request_extension & size_mask) != capacity ||
        (reply_extension & ~(parameter_mask | size_mask)) != (first_block | last_block) ||
        (reply_extension & parameter_mask) != (request_extension & parameter_mask))
        return PJ_IPC4_INVALID_HEADER;
    size = reply_extension & size_mask;
    if (size > capacity) return PJ_IPC4_BUFFER_SMALL;
    *bytes = size;
    return PJ_IPC4_VALID;
}

pj_ipc4_status pj_ipc4_fw_version(const PJ_BYTE *config, PJ_SIZE bytes,
    PJ_WORD version[4])
{
    const PJ_BYTE *value = 0;
    if (!config || !version || bytes > PJ_IPC4_MAX_MAILBOX_PAYLOAD)
        return PJ_IPC4_BAD_PARAMETER;
    while (bytes) {
        PJ_DWORD type, length;
        if (bytes < 8u) return PJ_IPC4_TRUNCATED;
        type = read_le32(config);
        length = read_le32(config + 4u);
        config += 8u;
        bytes -= 8u;
        if (length > bytes) return PJ_IPC4_TRUNCATED;
        /* SOF_IPC4_FW_CFG_FW_VERSION, struct sof_ipc4_fw_version. */
        if (type == 0u) {
            if (value || length != 8u) return PJ_IPC4_INVALID_HEADER;
            value = config;
        }
        config += length;
        bytes -= length;
    }
    if (!value) return PJ_IPC4_INVALID_HEADER;
    for (PJ_DWORD i = 0; i < 4u; ++i)
        version[i] = (PJ_WORD)(value[i * 2u] | ((PJ_WORD)value[i * 2u + 1u] << 8));
    return PJ_IPC4_VALID;
}

pj_ipc4_status pj_ipc4_fw_boot_caps(const PJ_BYTE *config, PJ_SIZE bytes,
    PJ_DWORD required_mailbox, PJ_DWORD required_pipelines, pj_ipc4_boot_caps *out)
{
    pj_ipc4_boot_caps result = {0};
    PJ_DWORD seen = 0u;
    pj_ipc4_status status;
    if (!config || !out || !required_mailbox ||
        required_mailbox > PJ_IPC4_MAX_MAILBOX_PAYLOAD || !required_pipelines)
        return PJ_IPC4_BAD_PARAMETER;
    status = pj_ipc4_fw_version(config, bytes, result.version);
    if (status != PJ_IPC4_VALID) return status;
    while (bytes) {
        PJ_DWORD type = read_le32(config), length = read_le32(config + 4u);
        PJ_DWORD bit = 0u, *target = 0;
        /* The version parser above has checked every tuple's span. */
        config += 8u;
        bytes -= 8u;
        switch (type) {
        case 6u: bit = 1u; target = &result.dl_bytes; break;
        case 7u: bit = 2u; target = &result.ul_bytes; break;
        case 9u: bit = 4u; target = &result.max_pipelines; break;
        default: break;
        }
        if (target) {
            if ((seen & bit) || length != 4u) return PJ_IPC4_INVALID_HEADER;
            *target = read_le32(config);
            seen |= bit;
        }
        config += length;
        bytes -= length;
    }
    if (seen != 7u) return PJ_IPC4_INVALID_HEADER;
    if (result.dl_bytes < required_mailbox || result.ul_bytes < required_mailbox ||
        result.max_pipelines < required_pipelines)
        return PJ_IPC4_BUFFER_SMALL;
    *out = result;
    return PJ_IPC4_VALID;
}
