/*
Copyright (C) 2016-2019 Draios Inc dba Sysdig.

This file is part of falco.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <inja/inja.hpp>

#include "falco_common.h"
#include "k8s_psp.h"
#include "json_evt.h"

using namespace falco;

k8s_psp_converter::k8s_psp_converter()
{
}

k8s_psp_converter::~k8s_psp_converter()
{
}

static std::string items_join(inja::Arguments& args)
{
	std::string delim = *(args.at(0));
	const nlohmann::json *items = args.at(1);

	std::string ret;
	bool first = true;

	for(auto &item : *items)
	{
		if(!first)
		{
			ret += delim;
		}
		first = false;

		ret += "\"";
		ret += item;
		ret += "\"";
	}

	return ret;
}

std::string k8s_psp_converter::generate_rules(const std::string &psp_yaml, const std::string &rules_template)
{
	load_yaml(psp_yaml);

	try {
		inja::Environment env;

		env.add_callback("join", 2, items_join);
		env.set_line_statement("DO_NOT_USE_LINE_STATEMENTS");

		return env.render(rules_template, m_params);
	}
	catch (const std::runtime_error &ex)
	{
		throw falco_exception(string("Could not render rules template: ") + ex.what());
	}
}

nlohmann::json k8s_psp_converter::parse_ranges(const YAML::Node &node, bool create_objs)
{
	nlohmann::json ret = nlohmann::json::array();

	for(auto &range : node)
	{
		if(create_objs)
		{
			nlohmann::json r;
			r["min"] = range["min"].as<string>();
			r["max"] = range["max"].as<string>();
			ret.push_back(r);
		}
		else
		{
			std::string r = range["min"].as<string>() +
				":" +
				range["max"].as<string>();

			ret.push_back(r);
		}
	}

	return ret;
}

nlohmann::json k8s_psp_converter::parse_sequence(const YAML::Node &node)
{
	nlohmann::json ret = nlohmann::json::array();

	for(auto &item : node)
	{
		ret.push_back(item.as<std::string>());
	}

	return ret;
}

void k8s_psp_converter::init_params(nlohmann::json &params)
{
	params.clear();
	params["policy_name"] = "unknown";
	params["image_list"] = nlohmann::json::array();
	params["allow_privileged"] = true;
	params["allow_host_pid"] = true;
	params["allow_host_ipc"] = true;
	params["allow_host_network"] = true;
	params["host_network_ports"] = nlohmann::json::array();
	params["allowed_volume_types"] = nlohmann::json::array();
	params["allowed_flexvolume_drivers"] = nlohmann::json::array();
	params["allowed_host_paths"] = nlohmann::json::array();
	params["must_run_fs_groups"] = nlohmann::json::array();
	params["may_run_fs_groups"] = nlohmann::json::array();
	params["must_run_as_users"] = nlohmann::json::array();
	params["must_run_as_users_objs"] = nlohmann::json::array();
	params["must_run_as_non_root"] = false;
	params["must_run_as_groups"] = nlohmann::json::array();
	params["must_run_as_groups_objs"] = nlohmann::json::array();
	params["may_run_as_groups"] = nlohmann::json::array();
	params["read_only_root_filesystem"] = false;
	params["must_run_supplemental_groups"] = nlohmann::json::array();
	params["may_run_supplemental_groups"] = nlohmann::json::array();
	params["allow_privilege_escalation"] = true;
	params["allowed_capabilities"] = nlohmann::json::array();
	params["allowed_proc_mount_types"] = nlohmann::json::array();
}

void k8s_psp_converter::load_yaml(const std::string &psp_yaml)
{
	try
	{
		init_params(m_params);

		YAML::Node root = YAML::Load(psp_yaml);

		if(!root["kind"] || root["kind"].as<std::string>() != "PodSecurityPolicy")
		{
			throw falco_exception("PSP Yaml Document does not have kind: PodSecurityPolicy");
		}

		if(!root["metadata"])
		{
			throw falco_exception("PSP Yaml Document does not have metadata property");
		}

		auto metadata = root["metadata"];

		if(!metadata["name"])
		{
			throw falco_exception("PSP Yaml Document does not have metadata: name");
		}

		m_params["policy_name"] = metadata["name"].as<std::string>();

		// The generated rules need a set of images for which
		// to scope the rules. A annotation with the key
		// "falco-rules-psp-images" provides the list of images.

		if(!metadata["annotations"] ||
		   !metadata["annotations"]["falco-rules-psp-images"])
		{
			throw falco_exception("PSP Yaml Document does not have an annotation \"falco-rules-psp-images\" that lists the images for which the generated rules should apply");
		}

		for(const auto &image : metadata["annotations"]["falco-rules-psp-images"])
		{
			m_params["image_list"].push_back(image.as<std::string>());
		}

		if(!root["spec"])
		{
			throw falco_exception("PSP Yaml Document does not have spec property");
		}

		auto spec = root["spec"];

		if(spec["privileged"])
		{
			m_params["allow_privileged"] = spec["privileged"].as<bool>();
		}

		if(spec["hostPID"])
		{
			m_params["allow_host_pid"] = spec["hostPID"].as<bool>();
		}

		if(spec["hostIPC"])
		{
			m_params["allow_host_ipc"] = spec["hostIPC"].as<bool>();
		}

		if(spec["hostNetwork"])
		{
			m_params["allow_host_network"] = spec["hostNetwork"].as<bool>();
		}

		if(spec["hostPorts"])
		{
			m_params["host_network_ports"] = parse_ranges(spec["hostPorts"]);

			// no_value is also allowed to account for
			// containers that do not have a hostPort
			m_params["host_network_ports"].push_back(json_event_filter_check::no_value);
		}

		if(spec["volumes"])
		{
			m_params["allowed_volume_types"] = parse_sequence(spec["volumes"]);

			// no_value is also allowed to account for
			// containers that do not have any volumes
			m_params["allowed_volume_types"].push_back(json_event_filter_check::no_value);
		}

		if(spec["allowedHostPaths"])
		{
			nlohmann::json items;
			for(const auto &hostpath : spec["allowedHostPaths"])
			{
				std::string path = hostpath["pathPrefix"].as<std::string>();
			        items.push_back(path);
			}

			m_params["allowed_host_paths"] = items;

			// no_value is also allowed to account for
			// containers that do not have any host path volumes
			m_params["allowed_host_paths"].push_back(json_event_filter_check::no_value);
		}

		if(spec["allowedFlexVolumes"])
		{
			nlohmann::json items;
			for(const auto &volume : spec["allowedFlexVolumes"])
			{
				items.push_back(volume["driver"].as<std::string>());
			}
			m_params["allowed_flexvolume_drivers"] = items;

			// no_value is also allowed to account for
			// containers that do not have any flexvolume drivers
			m_params["allowed_flexvolume_drivers"].push_back(json_event_filter_check::no_value);
		}

		if(spec["fsGroup"])
		{
			std::string rule = spec["fsGroup"]["rule"].as<std::string>();

			if(rule == "MustRunAs")
			{
				m_params["must_run_fs_groups"] = parse_ranges(spec["fsGroup"]["ranges"]);

				// *Not* adding no_value, as a fsGroup must be specified.
			}
			else if(rule == "MayRunAs")
			{
				m_params["may_run_fs_groups"] = parse_ranges(spec["fsGroup"]["ranges"]);

				// no_value is also allowed as not specifying a group is allowed
				m_params["may_run_fs_groups"].push_back(json_event_filter_check::no_value);

			}
			else if(rule == "RunAsAny")
			{
				// Do nothing, any allowed
			}
			else
			{
				throw std::invalid_argument("fsGroup rule \"" + rule + "\" was not one of MustRunAs/MayRunAs/RunAsAny");
			}
		}

		if(spec["runAsUser"])
		{
			std::string rule = spec["runAsUser"]["rule"].as<std::string>();

			if(rule == "MustRunAs")
			{
				m_params["must_run_as_users"] = parse_ranges(spec["runAsUser"]["ranges"]);

				bool create_objs=true;
				m_params["must_run_as_users_objs"] = parse_ranges(spec["runAsUser"]["ranges"], create_objs);

				// *Not* adding no_value, as a uid must be specified
			}
			else if (rule == "MustRunAsNonRoot")
			{
				m_params["must_run_as_non_root"] = true;
			}
			else if(rule == "RunAsAny")
			{
				// Do nothing, any allowed
			}
			else
			{
				throw std::invalid_argument("runAsUser rule \"" + rule + "\" was not one of MustRunAs/MustRunAsNonRoot/RunAsAny");
			}
		}

		if(spec["runAsGroup"])
		{
			std::string rule = spec["runAsGroup"]["rule"].as<std::string>();

			if(rule == "MustRunAs")
			{
				m_params["must_run_as_groups"] = parse_ranges(spec["runAsGroup"]["ranges"]);

				bool create_objs=true;
				m_params["must_run_as_groups_objs"] = parse_ranges(spec["runAsGroup"]["ranges"], create_objs);

				// *Not* adding no_value, as a gid must be specified
			}
			else if(rule == "MayRunAs")
			{
				m_params["may_run_as_groups"] = parse_ranges(spec["runAsGroup"]["ranges"]);

				// no_value is also allowed as not specifying a group is allowed
				m_params["may_run_as_groups"].push_back(json_event_filter_check::no_value);
			}
			else if(rule == "RunAsAny")
			{
				// Do nothing, any allowed
			}
			else
			{
				throw std::invalid_argument("runAsGroup rule \"" + rule + "\" was not one of MustRunAs/MayRunAs/RunAsAny");
			}
		}

		if(spec["readOnlyRootFilesystem"])
		{
			m_params["read_only_root_filesystem"] = spec["readOnlyRootFilesystem"].as<bool>();
		}

		if(spec["supplementalGroups"])
		{
			std::string rule = spec["supplementalGroups"]["rule"].as<std::string>();

			if(rule == "MustRunAs")
			{
				m_params["must_run_supplemental_groups"] = parse_ranges(spec["supplementalGroups"]["ranges"]);

				// *Not* adding no_value, as a gid must be specified
			}
			else if(rule == "MayRunAs")
			{
				m_params["may_run_supplemental_groups"] = parse_ranges(spec["supplementalGroups"]["ranges"]);

				// no_value is also allowed as not specifying a group is allowed
				m_params["may_run_supplemental_groups"].push_back(json_event_filter_check::no_value);
			}
			else if(rule == "RunAsAny")
			{
				// Do nothing, any allowed
			}
			else
			{
				throw std::invalid_argument("supplementalGroups rule \"" + rule + "\" was not one of MustRunAs/MayRunAs/RunAsAny");
			}
		}

		if(spec["allowPrivilegeEscalation"])
		{
			m_params["allow_privilege_escalation"] = spec["allowPrivilegeEscalation"].as<bool>();
		}

		if(spec["allowedCapabilities"])
		{
			m_params["allowed_capabilities"] = parse_sequence(spec["allowedCapabilities"]);

			// no_value is also allowed as a container may not add any additional capabilities
			m_params["allowed_capabilities"].push_back(json_event_filter_check::no_value);
		}

		if(spec["allowedProcMountTypes"])
		{
			m_params["allowed_proc_mount_types"] = parse_sequence(spec["allowedProcMountTypes"]);

			// no_value is also allowed as a container may not have any proc mount types
			m_params["allowed_proc_mount_types"].push_back(json_event_filter_check::no_value);
		}
	}
	catch (const std::invalid_argument &ex)
	{
		throw falco_exception(string("Could not parse PSP Yaml Document: ") + ex.what());
	}
	catch (const YAML::ParserException& ex)
	{
		throw falco_exception(string("Could not parse PSP Yaml Document: ") + ex.what());
	}
	catch (const YAML::BadConversion& ex)
	{
		throw falco_exception(string("Could not convert value from PSP Yaml Document: ") + ex.what());
	}
}


