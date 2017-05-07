#include "scene_loader.hpp"
#include "gltf.hpp"
#include "importers.hpp"
#define RAPIDJSON_ASSERT(x) do { if (!(x)) throw "JSON error"; } while(0)
#include "rapidjson/document.h"
#include "mesh_util.hpp"
#include "enum_cast.hpp"

using namespace std;
using namespace rapidjson;
using namespace Util;

namespace Granite
{

SceneLoader::SceneLoader()
{
	scene.reset(new Scene);
}

void SceneLoader::load_scene(const std::string &path)
{
	auto file = Filesystem::get().open(path);
	if (!file)
		throw runtime_error("Failed to load GLTF file.");

	auto length = file->get_size();
	auto *buffer = static_cast<const char *>(file->map());
	if (!buffer)
		throw runtime_error("Failed to map GLTF file.");

	string json(buffer, buffer + length);
	parse(path, json);
}

Scene::NodeHandle SceneLoader::build_tree_for_subscene(const SubsceneData &subscene)
{
	auto &parser = *subscene.parser;
	std::vector<Scene::NodeHandle> nodes;
	nodes.reserve(parser.get_nodes().size());

	for (auto &node : parser.get_nodes())
	{
		if (!node.joint)
		{
			Scene::NodeHandle nodeptr;
			if (node.has_skin)
				nodeptr = scene->create_skinned_node(parser.get_skins()[node.skin]);
			else
				nodeptr = scene->create_node();

			nodes.push_back(nodeptr);
			nodeptr->transform.translation = node.transform.translation;
			nodeptr->transform.rotation = node.transform.rotation;
			nodeptr->transform.scale = node.transform.scale;
		}
		else
			nodes.push_back({});
	}

	unsigned i = 0;
	for (auto &node : parser.get_nodes())
	{
		if (nodes[i])
		{
			for (auto &child : node.children)
				nodes[i]->add_child(nodes[child]);
			for (auto &mesh : node.meshes)
				scene->create_renderable(subscene.meshes[mesh], nodes[i].get());
		}
		i++;
	}

	auto root = scene->create_node();
	for (auto &node : nodes)
		if (node && !node->get_parent())
			root->add_child(node);

	return root;
}

void SceneLoader::parse(const std::string &path, const std::string &json)
{
	Document doc;
	doc.Parse(json);

	if (doc.HasParseError())
		throw logic_error("Failed to parse.");

	auto &scenes = doc["scenes"];
	for (auto itr = scenes.MemberBegin(); itr != scenes.MemberEnd(); ++itr)
	{
		auto gltf_path = Path::relpath(path, itr->value.GetString());
		auto &subscene = subscenes[itr->name.GetString()];
		subscene.parser.reset(new GLTF::Parser(gltf_path));
		auto &parser = *subscene.parser;

		for (auto &mesh : parser.get_meshes())
		{
			Importer::MaterialInfo default_material;
			default_material.uniform_base_color = vec4(0.0f, 1.0f, 0.0f, 1.0f);
			AbstractRenderableHandle renderable;

			bool skinned = mesh.attribute_layout[ecast(MeshAttribute::BoneIndex)].format != VK_FORMAT_UNDEFINED;
			if (skinned)
			{
				if (mesh.has_material)
					renderable = Util::make_abstract_handle<AbstractRenderable, ImportedSkinnedMesh>(mesh,
					                                                                                 parser.get_materials()[mesh.material_index]);
				else
					renderable = Util::make_abstract_handle<AbstractRenderable, ImportedSkinnedMesh>(mesh, default_material);
			}
			else
			{
				if (mesh.has_material)
					renderable = Util::make_abstract_handle<AbstractRenderable, ImportedMesh>(mesh,
					                                                                          parser.get_materials()[mesh.material_index]);
				else
					renderable = Util::make_abstract_handle<AbstractRenderable, ImportedMesh>(mesh, default_material);
			}
			subscene.meshes.push_back(renderable);
		}
	}

	vector<Scene::NodeHandle> hierarchy;
	auto &nodes = doc["nodes"];
	for (auto itr = nodes.Begin(); itr != nodes.End(); ++itr)
	{
		auto &elem = *itr;
		bool has_scene = elem.HasMember("scene");

		auto scene_itr = has_scene ? subscenes.find(elem["scene"].GetString()) : end(subscenes);
		if (has_scene && scene_itr == end(subscenes))
			throw logic_error("Scene does not exist.");

		vec3 stride = vec3(0.0f);
		uvec3 instance_size = uvec3(1);
		if (elem.HasMember("grid_stride"))
		{
			auto &grid_stride = elem["grid_stride"];
			stride = vec3(grid_stride[0].GetFloat(), grid_stride[1].GetFloat(), grid_stride[2].GetFloat());
		}

		if (elem.HasMember("grid_size"))
		{
			auto &grid_size = elem["grid_size"];
			instance_size = uvec3(grid_size[0].GetUint(), grid_size[1].GetUint(), grid_size[2].GetUint());
		}

		Transform transform;

		if (elem.HasMember("translation"))
		{
			auto &t = elem["translation"];
			transform.translation = vec3(t[0].GetFloat(), t[1].GetFloat(), t[2].GetFloat());
		}

		if (elem.HasMember("rotation"))
		{
			auto &r = elem["rotation"];
			transform.rotation = normalize(quat(r[3].GetFloat(), r[0].GetFloat(), r[1].GetFloat(), r[2].GetFloat()));
		}

		if (elem.HasMember("scale"))
		{
			auto &s = elem["scale"];
			transform.scale = vec3(s[0].GetFloat(), s[1].GetFloat(), s[2].GetFloat());
		}

		if (has_scene)
		{
			if (all(equal(instance_size, uvec3(1))))
				hierarchy.push_back(build_tree_for_subscene(scene_itr->second));
			else
			{
				auto subroot = scene->create_node();
				for (unsigned z = 0; z < instance_size.z; z++)
				{
					for (unsigned y = 0; y < instance_size.y; y++)
					{
						for (unsigned x = 0; x < instance_size.x; x++)
						{
							auto child = build_tree_for_subscene(scene_itr->second);
							child->transform.translation = vec3(x, y, z) * stride;
							subroot->add_child(child);
						}
					}
				}
				hierarchy.push_back(subroot);
			}
		}

		hierarchy.back()->transform = transform;
	}

	auto hier_itr = begin(hierarchy);
	for (auto itr = nodes.Begin(); itr != nodes.End(); ++itr, ++hier_itr)
	{
		auto &elem = *itr;
		if (elem.HasMember("children"))
		{
			auto &children = elem["children"];
			for (auto itr = children.Begin(); itr != children.End(); ++itr)
			{
				uint32_t index = itr->GetUint();
				(*hier_itr)->add_child(hierarchy[index]);
			}
		}
	}

	auto root = scene->create_node();
	for (auto &node : hierarchy)
		if (!node->get_parent())
			root->add_child(node);

	scene->set_root_node(root);

	if (doc.HasMember("background"))
	{
		auto texture_path = Path::relpath(path, doc["background"].GetString());
		auto skybox = Util::make_abstract_handle<AbstractRenderable, Skybox>(texture_path);
		scene->create_renderable(skybox, nullptr);
	}
}

}