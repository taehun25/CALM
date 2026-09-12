#!/usr/bin/env python3
"""Generate DDS XML profiles for large-payload Wi-Fi traffic."""

import argparse
import json
import math
import os
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from typing import Optional


XML_NS = "http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles"
ET.register_namespace("", XML_NS)
CYCLONEDDS_XML_NS = "https://cdds.io/config"


@dataclass(frozen=True)
class OptimizerParameters:
    publish_rate_hz: float
    payload_bytes: int
    link_throughput_bps: Optional[int]
    link_utilization: Optional[float]
    max_message_size: int
    heartbeat_period_ns: int
    history_cache_enabled: bool
    history_cache_samples: Optional[int]
    offered_load_bps: int
    allocated_link_bps: Optional[int]
    offered_to_allocated_ratio: Optional[float]


def compute_parameters(
        publish_rate_hz: float,
        payload_bytes: int,
        link_throughput_bps: Optional[int],
        link_utilization: Optional[float],
        max_message_size: int = 1472,
        history_cache_enabled: bool = True) -> OptimizerParameters:
    if publish_rate_hz <= 0:
        raise ValueError("publish_rate_hz must be positive")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    if not 512 <= max_message_size <= 65500:
        raise ValueError("max_message_size must be within [512, 65500]")

    heartbeat_period_ns = max(1, int(round(1e9 / (2.0 * publish_rate_hz))))
    offered_load_bps = int(round(publish_rate_hz * payload_bytes * 8.0))

    history_cache_samples = None
    allocated_link_bps = None
    offered_to_allocated_ratio = None
    if history_cache_enabled:
        if link_throughput_bps is None or link_throughput_bps <= 0:
            raise ValueError(
                "link_throughput_bps must be positive when history cache optimization is enabled")
        if link_utilization is None or not 0 < link_utilization <= 1:
            raise ValueError(
                "link_utilization must be within (0, 1] when history cache optimization is enabled")
        allocated_link_bps = int(link_throughput_bps * link_utilization)
        allocated_link_bytes_per_second = allocated_link_bps / 8.0
        history_cache_samples = max(
            1, math.floor(allocated_link_bytes_per_second / payload_bytes))
        offered_to_allocated_ratio = offered_load_bps / allocated_link_bps
    else:
        # Rules 1 and 2 do not consume externally measured link capacity.
        link_throughput_bps = None
        link_utilization = None

    return OptimizerParameters(
        publish_rate_hz=publish_rate_hz,
        payload_bytes=payload_bytes,
        link_throughput_bps=link_throughput_bps,
        link_utilization=link_utilization,
        max_message_size=max_message_size,
        heartbeat_period_ns=heartbeat_period_ns,
        history_cache_enabled=history_cache_enabled,
        history_cache_samples=history_cache_samples,
        offered_load_bps=offered_load_bps,
        allocated_link_bps=allocated_link_bps,
        offered_to_allocated_ratio=offered_to_allocated_ratio,
    )


def _add_duration(parent: ET.Element, name: str, nanoseconds: int) -> None:
    duration = ET.SubElement(parent, name)
    ET.SubElement(duration, "sec").text = str(nanoseconds // 1_000_000_000)
    ET.SubElement(duration, "nanosec").text = str(nanoseconds % 1_000_000_000)


def _add_transport(
        profiles: ET.Element,
        transport_id: str,
        interface_address: str,
        max_message_size: int,
        transport_buffer_size: Optional[int] = None) -> None:
    descriptors = ET.SubElement(profiles, "transport_descriptors")
    descriptor = ET.SubElement(descriptors, "transport_descriptor")
    ET.SubElement(descriptor, "transport_id").text = transport_id
    ET.SubElement(descriptor, "type").text = "UDPv4"
    if transport_buffer_size is not None:
        ET.SubElement(descriptor, "sendBufferSize").text = str(
            transport_buffer_size)
        ET.SubElement(descriptor, "receiveBufferSize").text = str(
            transport_buffer_size)
    ET.SubElement(descriptor, "maxMessageSize").text = str(max_message_size)
    whitelist = ET.SubElement(descriptor, "interfaceWhiteList")
    ET.SubElement(whitelist, "address").text = interface_address


def _add_participant(profiles: ET.Element, profile_name: str, transport_id: str) -> None:
    participant = ET.SubElement(
        profiles, "participant",
        {"profile_name": profile_name, "is_default_profile": "true"})
    rtps = ET.SubElement(participant, "rtps")
    ET.SubElement(rtps, "useBuiltinTransports").text = "false"
    transports = ET.SubElement(rtps, "userTransports")
    ET.SubElement(transports, "transport_id").text = transport_id


def _add_topic_limits(parent: ET.Element, history_cache_samples: int) -> None:
    topic = ET.SubElement(parent, "topic")
    history = ET.SubElement(topic, "historyQos")
    ET.SubElement(history, "kind").text = "KEEP_ALL"
    limits = ET.SubElement(topic, "resourceLimitsQos")
    ET.SubElement(limits, "max_samples").text = str(history_cache_samples)
    # The experiment topics are unkeyed, so one instance is sufficient.
    ET.SubElement(limits, "max_instances").text = "1"
    ET.SubElement(limits, "max_samples_per_instance").text = str(
        history_cache_samples)


def _add_dynamic_auxiliary_endpoint(
        profiles: ET.Element,
        endpoint_kind: str,
        profile_name: str) -> None:
    """Cover ROS-internal endpoints that exist beside the measured endpoint."""
    endpoint = ET.SubElement(
        profiles,
        endpoint_kind,
        {"profile_name": profile_name, "is_default_profile": "true"},
    )
    qos = ET.SubElement(endpoint, "qos")
    data_sharing = ET.SubElement(qos, "data_sharing")
    ET.SubElement(data_sharing, "kind").text = "OFF"
    ET.SubElement(endpoint, "historyMemoryPolicy").text = "DYNAMIC"


def _write_xml(root: ET.Element, output_path: str) -> None:
    output_dir = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(output_dir, exist_ok=True)
    tree = ET.ElementTree(root)
    ET.indent(tree, space="    ")
    tree.write(output_path, encoding="UTF-8", xml_declaration=True)


def generate_profiles(
        parameters: OptimizerParameters,
        publisher_address: str,
        subscriber_address: str,
        publisher_output: str,
        subscriber_output: str,
        max_blocking_time_sec: int = 1000,
        transport_buffer_size: Optional[int] = None) -> None:
    if max_blocking_time_sec <= 0:
        raise ValueError("max_blocking_time_sec must be positive")

    pub_root = ET.Element(f"{{{XML_NS}}}dds")
    pub_profiles = ET.SubElement(pub_root, "profiles")
    _add_transport(
        pub_profiles, "wireless_optimizer_pub_udp", publisher_address,
        parameters.max_message_size, transport_buffer_size)
    _add_participant(
        pub_profiles, "wireless_optimizer_pub_participant",
        "wireless_optimizer_pub_udp")

    publisher = ET.SubElement(
        pub_profiles, "publisher",
        {"profile_name": "wireless_optimizer_writer", "is_default_profile": "true"})
    if parameters.history_cache_enabled:
        assert parameters.history_cache_samples is not None
        _add_topic_limits(publisher, parameters.history_cache_samples)
    pub_qos = ET.SubElement(publisher, "qos")
    reliability = ET.SubElement(pub_qos, "reliability")
    ET.SubElement(reliability, "kind").text = "RELIABLE"
    max_blocking_time = ET.SubElement(reliability, "max_blocking_time")
    ET.SubElement(max_blocking_time, "sec").text = str(max_blocking_time_sec)
    ET.SubElement(max_blocking_time, "nanosec").text = "0"
    data_sharing = ET.SubElement(pub_qos, "data_sharing")
    ET.SubElement(data_sharing, "kind").text = "OFF"
    ET.SubElement(pub_qos, "disable_heartbeat_piggyback").text = "false"
    pub_times = ET.SubElement(publisher, "times")
    _add_duration(pub_times, "initialHeartbeatDelay", 0)
    _add_duration(pub_times, "heartbeatPeriod", parameters.heartbeat_period_ns)
    _add_duration(pub_times, "nackResponseDelay", 0)
    _add_duration(pub_times, "nackSupressionDuration", 0)
    ET.SubElement(publisher, "historyMemoryPolicy").text = "DYNAMIC"
    _add_dynamic_auxiliary_endpoint(
        pub_profiles, "subscriber", "wireless_optimizer_pub_aux_reader")
    _write_xml(pub_root, publisher_output)

    sub_root = ET.Element(f"{{{XML_NS}}}dds")
    sub_profiles = ET.SubElement(sub_root, "profiles")
    _add_transport(
        sub_profiles, "wireless_optimizer_sub_udp", subscriber_address,
        parameters.max_message_size, transport_buffer_size)
    _add_participant(
        sub_profiles, "wireless_optimizer_sub_participant",
        "wireless_optimizer_sub_udp")

    subscriber = ET.SubElement(
        sub_profiles, "subscriber",
        {"profile_name": "wireless_optimizer_reader", "is_default_profile": "true"})
    if parameters.history_cache_enabled:
        assert parameters.history_cache_samples is not None
        _add_topic_limits(subscriber, parameters.history_cache_samples)
    sub_qos = ET.SubElement(subscriber, "qos")
    sub_reliability = ET.SubElement(sub_qos, "reliability")
    ET.SubElement(sub_reliability, "kind").text = "RELIABLE"
    sub_data_sharing = ET.SubElement(sub_qos, "data_sharing")
    ET.SubElement(sub_data_sharing, "kind").text = "OFF"
    sub_times = ET.SubElement(subscriber, "times")
    _add_duration(sub_times, "initialAcknackDelay", 0)
    _add_duration(sub_times, "heartbeatResponseDelay", 0)
    ET.SubElement(subscriber, "historyMemoryPolicy").text = "DYNAMIC"
    _add_dynamic_auxiliary_endpoint(
        sub_profiles, "publisher", "wireless_optimizer_sub_aux_writer")
    _write_xml(sub_root, subscriber_output)


def generate_opt1_profiles(
        publisher_address: str,
        subscriber_address: str,
        publisher_output: str,
        subscriber_output: str,
        max_message_size: int = 1472,
        transport_buffer_size: Optional[int] = None) -> None:
    """Generate Fast DDS profiles with only the MTU-safe message-size rule.

    No heartbeat, ACKNACK, NACK response, history-cache, or blocking-time
    parameters are changed. This keeps OPT1 isolated from OPT2 and OPT3.
    """
    if not 512 <= max_message_size <= 65500:
        raise ValueError("max_message_size must be within [512, 65500]")

    def build_profile(address: str, role: str) -> ET.Element:
        root = ET.Element(f"{{{XML_NS}}}dds")
        profiles = ET.SubElement(root, "profiles")
        transport_id = f"wireless_opt1_{role}_udp"
        _add_transport(
            profiles, transport_id, address, max_message_size,
            transport_buffer_size)
        _add_participant(
            profiles, f"wireless_opt1_{role}_participant", transport_id)

        _add_dynamic_auxiliary_endpoint(
            profiles, "publisher", f"wireless_opt1_{role}_writer")
        _add_dynamic_auxiliary_endpoint(
            profiles, "subscriber", f"wireless_opt1_{role}_reader")
        return root

    _write_xml(build_profile(publisher_address, "pub"), publisher_output)
    _write_xml(build_profile(subscriber_address, "sub"), subscriber_output)


def _cyclonedds_profile(
        parameters: OptimizerParameters,
        interface_address: str) -> ET.Element:
    """Build the Cyclone DDS analogue of optimizer rules 1 and 2.

    Cyclone DDS has no Fast DDS writer/reader profile split. Each process gets
    a domain configuration that pins its interface, limits generated UDP
    payloads, and sets the adaptive heartbeat interval bounds to the value
    derived from the publish rate.
    """
    root = ET.Element("CycloneDDS", {"xmlns": CYCLONEDDS_XML_NS})
    domain = ET.SubElement(root, "Domain", {"Id": "any"})
    general = ET.SubElement(domain, "General")
    interfaces = ET.SubElement(general, "Interfaces")
    ET.SubElement(
        interfaces,
        "NetworkInterface",
        {
            "address": interface_address,
            "priority": "default",
            "multicast": "default",
        },
    )
    ET.SubElement(general, "AllowMulticast").text = "default"
    ET.SubElement(general, "MaxMessageSize").text = (
        f"{parameters.max_message_size} B"
    )
    ET.SubElement(general, "MaxRexmitMessageSize").text = (
        f"{parameters.max_message_size} B"
    )

    heartbeat = f"{parameters.heartbeat_period_ns} ns"
    internal = ET.SubElement(domain, "Internal")
    ET.SubElement(
        internal,
        "HeartbeatInterval",
        {"min": heartbeat, "minsched": heartbeat, "max": heartbeat},
    ).text = heartbeat
    return root


def generate_cyclonedds_profiles(
        parameters: OptimizerParameters,
        publisher_address: str,
        subscriber_address: str,
        publisher_output: str,
        subscriber_output: str) -> None:
    """Generate per-host Cyclone DDS profiles for optimizer rules 1 and 2."""
    if parameters.history_cache_enabled:
        raise ValueError(
            "Cyclone DDS history-cache optimization is not equivalent to the "
            "Fast DDS resource-limit rule; use rules 1 and 2 only"
        )
    _write_xml(
        _cyclonedds_profile(parameters, publisher_address), publisher_output
    )
    _write_xml(
        _cyclonedds_profile(parameters, subscriber_address), subscriber_output
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Fast DDS wireless large-payload optimizer profiles")
    parser.add_argument("--publish-rate-hz", type=float, required=True)
    parser.add_argument("--payload-bytes", type=int, required=True)
    parser.add_argument("--link-throughput-bps", type=int)
    parser.add_argument("--link-utilization", type=float, default=0.6)
    parser.add_argument(
        "--disable-history-cache-optimization", action="store_true",
        help="Apply only maxMessageSize and heartbeat optimization; leave history limits at defaults")
    parser.add_argument("--max-message-size", type=int, default=1472)
    parser.add_argument("--publisher-address", required=True)
    parser.add_argument("--subscriber-address", required=True)
    parser.add_argument("--publisher-output", required=True)
    parser.add_argument("--subscriber-output", required=True)
    parser.add_argument("--max-blocking-time-sec", type=int, default=1000)
    parser.add_argument("--manifest-output")
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    parameters = compute_parameters(
        args.publish_rate_hz,
        args.payload_bytes,
        args.link_throughput_bps,
        args.link_utilization,
        args.max_message_size,
        history_cache_enabled=not args.disable_history_cache_optimization)
    generate_profiles(
        parameters,
        args.publisher_address,
        args.subscriber_address,
        args.publisher_output,
        args.subscriber_output,
        args.max_blocking_time_sec)

    manifest = asdict(parameters)
    if args.manifest_output:
        output_dir = os.path.dirname(os.path.abspath(args.manifest_output))
        os.makedirs(output_dir, exist_ok=True)
        with open(args.manifest_output, "w", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
    print(json.dumps(manifest, sort_keys=True))


if __name__ == "__main__":
    main()
