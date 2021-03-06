#include "webgl.h"
#include "affixes.h"
#include "textures.h"
#include "types/GameBalance.h"
#include "types/Actor.h"
#include "types/Particle.h"
#include "types/AnimSet.h"
#include "itemlib.h"
#include "strings.h"
#include <set>
#include <bitset>
#include <algorithm>

static Vector Read(Anim::Type::DT_VECTOR3D const& v) {
  return Vector(v.x00_X, v.x04_Y, v.x08_Z);
}
static Vector Read(Appearance::Type::DT_VECTOR3D const& v) {
  return Vector(v.x00_X, v.x04_Y, v.x08_Z);
}
static Quaternion Read(Appearance::Type::Quaternion const& q) {
  return Quaternion(q.x00_DT_VECTOR3D.x00_X, q.x00_DT_VECTOR3D.x04_Y,
    q.x00_DT_VECTOR3D.x08_Z, q.x0C);
}
static Quaternion Read(Anim::Type::Quaternion16 const& q) {
  return Quaternion(q.x00.value(), q.x02.value(), q.x04.value(), q.x06.value());
}

std::map<std::string, uint32> get_item_icons(bool leg = false);

bool isParent(Appearance::Type::Structure& data, int from, int to) {
  while (from != to && from >= 0) {
    from = data.x010_BoneStructures[from].x040;
  }
  return from == to;
}
bool isRelated(Appearance::Type::Structure& data, int a, int b) {
  return isParent(data, a, b) || isParent(data, b, a);
}

namespace WebGL {
  struct Triangle {
    Index verts[3];
    BoneIndex bones[9];
    uint32 numBones = 0;
    uint32 group = -1;
    uint32 unused;
  };
  static const int MAX_BONES = 128;
  static const int MAX_GROUP_BONES = 24;
  struct TriangleSet {
    typedef std::pair<int, Triangle*> Ref;
    std::set<Ref> queue;
    std::vector<Triangle*> list[MAX_BONES];
    void add(Triangle* tri) {
      for (uint32 i = 0; i < tri->numBones; ++i) {
        list[tri->bones[i]].push_back(tri);
      }
      queue.emplace(tri->unused = tri->numBones, tri);
    }
    Triangle* pop() {
      auto it = queue.begin();
      Triangle* tri = it->second;
      queue.erase(it);
      return tri;
    }
    void reduce(BoneIndex bone) {
      for (Triangle* tri : list[bone]) {
        if (tri->group != -1) continue;
        auto it = queue.find(Ref(tri->unused, tri));
        if (it == queue.end()) throw Exception("uh oh");
        queue.erase(it);
        queue.emplace(--tri->unused, tri);
      }
    }
    bool empty() const {
      return queue.empty();
    }
  };

  struct ObjectData {
    struct Group {
      std::bitset<MAX_BONES> bones;
      std::map<Index, Index> vertices;
      std::vector<Index> indices;
    };
    std::vector<Group> groups;
    Appearance::Type::SubObject& original;

    ObjectData(Appearance::Type::SubObject& object, uint32 maxBones)
      : original(object)
    {
      if (!object.x020_VertInfluences.size()) {
        groups.emplace_back();
        auto& group = groups[0];
        for (uint32 i = 0; i < maxBones; ++i) group.bones.set(i);
        for (uint32 i = 0; i < object.x010_FatVertexs.size(); ++i) group.vertices[i] = i;
        for (auto idx : object.x038_short) group.indices.push_back(idx);
        return;
      }

      std::vector<Triangle> triangles;
      for (uint32 i = 0; i + 3 <= object.x038_short.size(); i += 3) {
        triangles.emplace_back();
        auto& tri = triangles.back();
        for (uint32 j = 0; j < 3; ++j) {
          tri.verts[j] = object.x038_short[i + j];
          for (auto& inf : object.x020_VertInfluences[tri.verts[j]].x00_Influences) {
            if (inf.x04) tri.bones[tri.numBones++] = inf.x00;
          }
        }
        std::sort(tri.bones, tri.bones + tri.numBones);
        tri.numBones = std::unique(tri.bones, tri.bones + tri.numBones) - tri.bones;
      }
      while (true) {
        TriangleSet tris;
        for (auto& tri : triangles) {
          if (tri.group == -1) {
            tris.add(&tri);
          }
        }
        if (tris.empty()) break;
        groups.emplace_back();
        auto& group = groups.back();
        uint32 used = 0;
        while (!tris.empty()) {
          Triangle* tri = tris.pop();
          if (tri->unused + used > MAX_GROUP_BONES) break;
          for (uint16 v : tri->verts) {
            auto it = group.vertices.find(v);
            if (it == group.vertices.end()) {
              Index index = group.vertices.size();
              group.vertices[v] = index;
              group.indices.push_back(index);
            } else {
              group.indices.push_back(it->second);
            }
          }
          tri->group = groups.size() - 1;
          for (uint32 i = 0; i < tri->numBones; ++i) {
            if (!group.bones.test(tri->bones[i])) {
              tris.reduce(tri->bones[i]);
              ++used;
              group.bones.set(tri->bones[i]);
            }
          }
        }
      }
    }
  };

  static Archive* texArchive = nullptr;

  uint32 AddTexture(uint32 texId, bool gray = false) {
    if (texArchive && texId != -1 && !texArchive->has(texId)) {
      Image image = GameTextures::get(texId);
      if (image) image.write(texArchive->create(texId), gray ? ImageFormat::PNGGrayscale : ImageFormat::PNG);
    }
    return texId == -1 ? 0 : texId;
  }
  void DoWriteModel(File& file, SnoFile<Appearance>& app) {
    ModelHeader header;
    header.numBones = app->x010_Structure.x010_BoneStructures.size();
    header.numHardpoints = app->x010_Structure.x0F0_Hardpoints.size();
    header.numAppearances = app->x1C0_AppearanceLooks.size();
    header.numMaterials = app->x1B0_AppearanceMaterials.size();
    header.numObjects = app->x010_Structure.x088_GeoSets[0].x10_SubObjects.size();
    header.boneOffset = sizeof header;
    header.hardpointOffset = header.boneOffset + header.numBones * sizeof(Bone);
    header.objectOffset = header.hardpointOffset + header.numHardpoints * sizeof(Hardpoint);
    header.materialOffset = header.objectOffset + header.numObjects * sizeof(Object);
    uint32 vCount = 0;
    for (auto& object : app->x010_Structure.x088_GeoSets[0].x10_SubObjects) {
      for (auto& v : object.x010_FatVertexs) {
        header.center += Read(v.x00_Position);
        ++vCount;
      }
    }
    if (vCount) header.center /= static_cast<float>(vCount);
    uint32 fileSize = header.materialOffset + sizeof(Material) * header.numAppearances * header.numMaterials;
    file.write(header);
    std::vector<Matrix> bones;
    for (auto& src : app->x010_Structure.x010_BoneStructures) {
      Bone dst;
      memset(dst.name, 0, sizeof dst.name);
      strcpy(dst.name, src.x000_Text);
      dst.parent = src.x040;
      dst.transform.translate = Read(src.x06C_PRSTransform.x10_DT_VECTOR3D);
      dst.transform.rotate = Read(src.x06C_PRSTransform.x00_Quaternion);
      dst.transform.scale = src.x06C_PRSTransform.x1C;
      dst.capsuleOffset = 0;
      dst.constraintOffset = 0;
      bones.push_back(Matrix::scale(1.0f / dst.transform.scale) *
        dst.transform.rotate.conj().matrix() * Matrix::translate(-dst.transform.translate));
      if (src.x118_CollisionShapes.size()) {
        dst.capsuleOffset = fileSize;
        fileSize += sizeof(CapsuleInfo);
      }
      if (src.x128_ConstraintParameters.size()) {
        dst.constraintOffset = fileSize;
        fileSize += sizeof(Constraint);
      }
      file.write(dst);
    }
    for (auto& src : app->x010_Structure.x0F0_Hardpoints) {
      Hardpoint dst;
      memset(dst.name, 0, sizeof dst.name);
      strcpy(dst.name, src.x00_Text);
      dst.parent = src.x40;
      dst.transform = Matrix::translate(Read(src.x44_PRTransform.x10_DT_VECTOR3D)) *
                      Read(src.x44_PRTransform.x00_Quaternion).matrix();
      if (dst.parent != -1) dst.transform = bones[dst.parent] * dst.transform;
      dst.transform.transpose();
      file.write(dst);
    }
    std::map<std::string, uint32> materials;
    for (uint32 i = 0; i < header.numMaterials; ++i) {
      materials[app->x1B0_AppearanceMaterials[i].x00_Text] = i;
    }
    std::vector<ObjectData> objects;
    for (auto& src : app->x010_Structure.x088_GeoSets[0].x10_SubObjects) {
      objects.emplace_back(src, app->x010_Structure.x010_BoneStructures.size());
      auto& data = objects.back();
      Object dst;
      dst.material = materials[src.x05C_Text];
      dst.numGroups = data.groups.size();
      dst.groupOffset = fileSize;
      fileSize += sizeof(ObjectGroup) * dst.numGroups;
      file.write(dst);
    }
    for (auto& mat : app->x1B0_AppearanceMaterials) {
      for (auto& sub : mat.x88_SubObjectAppearances) {
        uint32 fpos = file.tell();
        uint32 texDiffuse = -1, texSpecular = -1, texTintBase = -1, texTintMask = -1;
        for (auto& tex : sub.x18_UberMaterial.x58_MaterialTextureEntries) {
          switch (tex.x00) {
          case 1:
            texDiffuse = tex.x08_MaterialTexture.x00_TexturesSno;
            break;
          case 5:
            texSpecular = tex.x08_MaterialTexture.x00_TexturesSno;
            break;
          case 11:
            texTintBase = tex.x08_MaterialTexture.x00_TexturesSno;
            break;
          case 54:
            texTintMask = tex.x08_MaterialTexture.x00_TexturesSno;
            break;
          }
        }
        float alpha = 1.0f;
        for (size_t i = 0; i < sub.x10_TagMap.size() - 1; ++i) {
          if (sub.x10_TagMap[i] == 196864) {
            alpha = *(float*) &sub.x10_TagMap[i + 1];
          }
        }
        Material dst;
        dst.diffuse = AddTexture(texDiffuse);
        dst.specular = AddTexture(texSpecular);
        dst.tintBase = AddTexture(texTintBase);
        dst.tintMask = AddTexture(texTintMask);
        file.write(dst);
      }
    }
    for (auto& src : app->x010_Structure.x010_BoneStructures) {
      if (src.x118_CollisionShapes.size()) {
        CapsuleInfo dst;
        auto& col = src.x118_CollisionShapes[0];
        dst.start = Read(col.x30_DT_VECTOR3D);
        dst.end = Read(col.x3C_DT_VECTOR3D);
        dst.radius = col.x48;
        file.write(dst);
      }
      if (src.x128_ConstraintParameters.size()) {
        Constraint dst;
        auto& data = src.x128_ConstraintParameters[0];
        dst.parent.rotate = Read(data.x078_PRTransform.x00_Quaternion);
        dst.parent.translate = Read(data.x078_PRTransform.x10_DT_VECTOR3D);
        dst.local.rotate = Read(data.x094_PRTransform.x00_Quaternion);
        dst.local.translate = Read(data.x094_PRTransform.x10_DT_VECTOR3D);
        dst.angles[0] = data.x0B0;
        dst.angles[1] = data.x0B4;
        dst.angles[2] = data.x0B8;
        dst.angles[3] = data.x0BC;
        dst.angles[4] = data.x0C0;
        file.write(dst);
      }
    }
    for (auto& object : objects) {
      for (auto& group : object.groups) {
        uint32 pos = file.tell();
        ObjectGroup dst;
        dst.numBones = group.bones.count();
        dst.boneOffset = fileSize;
        fileSize += sizeof(BoneIndex) * dst.numBones;
        dst.numVertices = group.vertices.size();
        dst.vertexOffset = fileSize;
        fileSize += sizeof(Vertex) * dst.numVertices;
        dst.numIndices = group.indices.size();
        dst.indexOffset = fileSize;
        fileSize += sizeof(Index) * ((dst.numIndices + 1) / 2) * 2;
        file.write(dst);
      }
    }
    for (auto& object : objects) {
      for (auto& group : object.groups) {
        uint32 boneMap[MAX_BONES];
        uint32 numBones = 0;
        for (uint32 i = 0; i < app->x010_Structure.x010_BoneStructures.size(); ++i) {
          if (group.bones.test(i)) {
            file.write(BoneIndex(i));
            boneMap[i] = numBones++;
          }
        }
        std::vector<uint32> vertices(group.vertices.size());
        for (auto it : group.vertices) {
          vertices[it.second] = it.first;
        }
        for (uint32 index : vertices) {
          auto& src = object.original.x010_FatVertexs[index];
          auto* inf = (object.original.x020_VertInfluences.size() > index ?
            &object.original.x020_VertInfluences[index].x00_Influences : nullptr);
          Vertex dst;
          memset(&dst, 0, sizeof dst);
          dst.position = Read(src.x00_Position);
          dst.normal[0] = src.x0C_Normal.x00_X - 128;
          dst.normal[1] = src.x0C_Normal.x01_Y - 128;
          dst.normal[2] = src.x0C_Normal.x02_Z - 128;
          dst.texcoord[0] = src.x18_TexCoords[0].x00_U - 0x8000;
          dst.texcoord[1] = src.x18_TexCoords[0].x02_V - 0x8000;
          if (inf) {
            for (uint32 j = 0; j < 3; ++j) {
              dst.bone_idx[j] = boneMap[(*inf)[j].x00];
              dst.bone_weight[j] = (*inf)[j].x04;
            }
          } else {
            dst.bone_weight[0] = 1;
          }
          file.write(dst);
        }
        file.write(group.indices.data(), group.indices.size() * sizeof(Index));
        if (group.indices.size() & 1) {
          file.write16(0);
        }
      }
    }
  }
  void WriteModel(std::string const& name) {
    DoWriteModel(File("WebGL" / name + ".model", "wb"), SnoFile<Appearance>(name));
  }

  void DoWriteAnimation(File& file, Anim::Type& anim) {
    auto& perm = anim.x28_AnimPermutations[0];
    AnimationSequence header;
    header.numFrames = perm.x090;
    header.velocity = perm.x048_Velocity;
    header.numBones = perm.x088_BoneNames.size();
    header.animationOffset = sizeof header;
    uint32 fileSize = header.animationOffset + header.numBones * sizeof(AnimationCurve);
    file.write(header);
    for (size_t i = 0; i < header.numBones; ++i) {
      AnimationCurve curve;
      memset(&curve, 0, sizeof curve);
      strcpy(curve.bone, perm.x088_BoneNames[i].x00_Text);
      curve.numTranslations = perm.x0A0_TranslationCurves[i].x10_TranslationKeies.size();
      curve.numRotations = perm.x0B0_RotationCurves[i].x10_RotationKeies.size();
      curve.numScales = perm.x0C0_ScaleCurves[i].x10_ScaleKeies.size();
      curve.translationOffset = fileSize;
      fileSize += curve.numTranslations * sizeof(TranslationKey);
      curve.rotationOffset = fileSize;
      fileSize += curve.numRotations * sizeof(RotationKey);
      curve.scaleOffset = fileSize;
      fileSize += curve.numScales * sizeof(ScaleKey);
      file.write(curve);
    }
    for (size_t i = 0; i < header.numBones; ++i) {
      for (auto& key : perm.x0A0_TranslationCurves[i].x10_TranslationKeies) {
        file.write(key.x00);
        file.write(Read(key.x04_DT_VECTOR3D));
      }
      for (auto& key : perm.x0B0_RotationCurves[i].x10_RotationKeies) {
        file.write(key.x00);
        file.write(Read(key.x04_Quaternion16));
      }
      for (auto& key : perm.x0C0_ScaleCurves[i].x10_ScaleKeies) {
        file.write(key.x00);
        file.write(key.x04);
      }
    }
  }

  void WriteAnimation(std::string const& name) {
    SnoFile<Anim> anim(name);
    if (!anim) return;
    File file("WebGL" / name + ".anim", "wb");
    DoWriteAnimation(file, *anim);
  }

  void DumpActorData(Archive& mdl, Archive& ani, uint32 aid, uint32 raid = 0) {
    SnoFile<Actor> actor(Actor::name(aid));
    if (!actor) return;
    SnoFile<Appearance> app(actor->x014_AppearanceSno.name());
    if (app) DoWriteModel(mdl.create(raid ? raid : aid), app);
    File animFile = SnoLoader::Load<AnimSet>(actor->x068_AnimSetSno.name());
    if (animFile) {
      json::Value value;
      json::Visitor::printExStrings = false;
      json::BuilderVisitor visitor(value);
      AnimSet::parse(animFile, &visitor);
      visitor.onEnd();
      for (auto& sub : value) {
        if (sub.type() != json::Value::tObject) continue;
        for (auto& val : sub) {
          uint32 id = val.getInteger();
          SnoFile<Anim> anim(Anim::name(id));
          if (anim) {
            DoWriteAnimation(ani.create(id), *anim);
          }
        }
      }
    }
  }

  uint32 FixEmitter(SnoFile<Actor>& actor) {
    if (1 || actor->x014_AppearanceSno.name() == "Emitter") {
      if (!actor->x080_MsgTriggeredEvents.size()) return actor->x000_Header.id;
      auto& name = actor->x080_MsgTriggeredEvents[0].x004_TriggerEvent.x02C_SNOName;
      if (name.type() != "Particle") return actor->x000_Header.id;
      SnoFile<Particle> particle(name.name());
      if (!particle) return actor->x000_Header.id;
      if (!Actor::name(particle->x338_ActorSno)) return actor->x000_Header.id;
      //printf("Fixing emitter: %s -> %s\n", actor.name().c_str(), Actor::name(particle->x338_ActorSno));
      return particle->x338_ActorSno;
    }
  }

  bool DumpItemActor(Archive& mdl, Archive& ani, std::set<uint32>& done, uint32 aid, bool fixEmitter, uint32 raid = 0) {
    if (done.count(aid)) return true;
    SnoFile<Actor> actor(Actor::name(aid));
    if (!actor) return false;
    done.insert(aid);
    std::map<uint32, uint32> tags;
    for (uint32 i = 1; i + 3 <= actor->x060_TagMap.size(); i += 3) {
      tags[actor->x060_TagMap[i + 1]] = actor->x060_TagMap[i + 2];
    }
    if (tags[94240] && !raid) {
      bool hasSelf = false;
      bool hasAny = false;
      for (uint32 id = 94208; id <= 94219; ++id) {
        if (tags[id] == aid) { hasSelf = true; continue; }
        if (DumpItemActor(mdl, ani, done, tags[id], fixEmitter, tags[id])) hasAny = true;
      }
      for (uint32 id = 94720; id <= 94731; ++id) {
        if (tags[id] == aid) { hasSelf = true; continue; }
        if (DumpItemActor(mdl, ani, done, tags[id], fixEmitter, tags[id])) hasAny = true;
      }
      if (!hasSelf && hasAny) return true;
    }
    DumpActorData(mdl, ani, fixEmitter ? FixEmitter(actor) : aid, raid ? raid : aid);
    return true;
  }
  void DumpActorLook(json::Value& value, uint32 aid) {
    SnoFile<Actor> actor(Actor::name(aid));
    if (!actor) return;
    SnoFile<Appearance> app(actor->x014_AppearanceSno.name());
    if (!app) return;
    auto& val = value[fmtstring("%d", aid)]["looks"];
    uint32 index = 0;
    for (auto& look : app->x1C0_AppearanceLooks) {
      val[fmtstring("%u", HashName(look.x00_Text))] = index++;
    }
  }
  void ClassInfo() {
    SnoFile<GameBalance> gmb("Characters");
    json::Value value;
    for (auto& hero : gmb->x088_Heros) {
      DumpActorLook(value, hero.x108_ActorSno);
      DumpActorLook(value, hero.x10C_ActorSno);
    }
    json::write(File("webgl_look.js", "w"), value, json::mJS);
  }
  void AddActorInfo(json::Value& values, uint32 aid, bool hair, bool fixEmitter, uint32 orig = 0) {
    SnoFile<Actor> actor(Actor::name(aid));
    if (!actor) return;
    uint32 emitId = (fixEmitter ? FixEmitter(actor) : aid);
    if (emitId != actor->x000_Header.id) {
      AddActorInfo(values, emitId, hair, false, aid);
      return;
    }
    auto& value = values[fmtstring("%d", orig ? orig : aid)];
    if (hair) {
      std::map<uint32, uint32> tags;
      for (uint32 i = 1; i + 3 <= actor->x060_TagMap.size(); i += 3) {
        tags[actor->x060_TagMap[i + 1]] = actor->x060_TagMap[i + 2];
      }
      value["hair"] = tags[66564];
    }
    SnoFile<AnimSet> animSet(actor->x068_AnimSetSno.name());
    if (animSet) {
      uint32 anim = 0;
      for (auto& tm : animSet->x010_AnimSetTagMaps) {
        if (!tm.x08_TagMap.size() || tm.x08_TagMap.size() != tm.x08_TagMap[0] * 3 + 1) continue;
        for (uint32 i = 1; i + 3 <= tm.x08_TagMap.size(); i += 3) {
          if (Anim::name(tm.x08_TagMap[i + 2])) {
            anim = tm.x08_TagMap[i + 2];
          }
        }
      }
      if (anim) {
        value["animation"] = anim;
      }
    }
    SnoFile<Appearance> app(actor->x014_AppearanceSno.name());
    if (app) {
      json::Value enable(json::Value::tObject);
      uint32 index = 0;
      for (auto& object : app->x010_Structure.x088_GeoSets[0].x10_SubObjects) {
        if (strcmp(object.x05C_Text, "FX_EMIT")) {
          enable[fmtstring("%d", index)] = 0;
        }
        ++index;
      }
      value["enable"] = enable;
    }
  }
  json::Value MakeActor(SnoFile<Actor>& actor, std::map<uint32, uint32>& tags, json::Value& actors, bool fixEmitter, bool right = false, bool hair = false) {
    static int charActors[] = {3301, 3285, 6544, 6526, 6485, 6481, 4721, 4717, 75207, 74706, 238284, 238286};
    if (tags[94240]) {
      json::Value res(json::Value::tObject);
      for (uint32 id = (right ? 94720 : 94208), idx = 0; idx < 12; ++id, ++idx) {
        if (Actor::name(tags[id])) {
          res[fmtstring("%d", charActors[idx])] = tags[id];
          AddActorInfo(actors, tags[id], hair, fixEmitter);
        }
      }
      return res;
    } else {
      AddActorInfo(actors, actor->x000_Header.id, hair, fixEmitter);
      return actor->x000_Header.id;
    }
  }
  void FillItemInfo(json::Value& dst, json::Value& actors, GameBalance::Type::Item& item, std::string const& type, std::string const& slot) {
    SnoFile<Actor> actor(item.x108_ActorSno.name());
    if (!actor) return;
    std::map<uint32, uint32> tags;
    for (uint32 i = 1; i + 3 <= actor->x060_TagMap.size(); i += 3) {
      tags[actor->x060_TagMap[i + 1]] = actor->x060_TagMap[i + 2];
    }
    uint32 aid = 0;
    if (slot == "head") {
      dst["actor"] = MakeActor(actor, tags, actors, false, false, true);
    }
    if (slot == "legs" || slot == "feet" || slot == "torso" || slot == "hands") {
      dst["armortype"] = tags[66560];
      dst["look"] = tags[66561];
    }
    if (slot == "shoulders") {
      dst["actor"] = MakeActor(actor, tags, actors, false);
      dst["actor_r"] = MakeActor(actor, tags, actors, false, true);
    }
    if (slot == "mainhand" || slot == "offhand" || slot == "twohand" || slot == "onehand") {
      if (type == "quiver") return;
      dst["actor"] = MakeActor(actor, tags, actors, type == "source" || type == "mojo");
    }
  }
  void ItemInfo() {
    json::Value out;
    json::Value items;
    json::Value actors;
    json::parse(File("webgl_actors.js"), actors, json::mJS);
    json::parse(File("itemtypes.js"), items, json::mJS);
    Logger::begin(items["itemById"].getMap().size(), "Dumping items");
    std::set<uint32> done;
    for (auto& kv : items["itemById"].getMap()) {
      Logger::item(kv.first.c_str());
      std::string type = kv.second["type"].getString();
      std::string slot = items["itemTypes"][type]["slot"].getString();
      auto* item = ItemLibrary::get(kv.first);
      if (!item) continue;
      auto& dst = out[kv.first];
      FillItemInfo(dst, actors, *item, type, slot);
    }
    json::Value final;
    for (auto& kv : out.getMap()) {
      if (kv.second.type() == json::Value::tObject) {
        final[kv.first] = kv.second;
      }
    }
    json::write(File("webgl_items.js", "w"), final, json::mJS);
    json::write(File("webgl_actors.js", "w"), actors, json::mJS);
  }

  void DumpActorSets(Archive& ans, uint32 aid) {
    SnoFile<Actor> actor(Actor::name(aid));
    if (!actor) return;
    SnoFile<AnimSet> animSet(actor->x068_AnimSetSno.name());
    if (!animSet) return;
    File& dst = ans.create(aid);
    for (auto& tm : animSet->x010_AnimSetTagMaps) {
      uint32 count = tm.x08_TagMap[0];
      if (!count || tm.x08_TagMap.size() != count * 3 + 1) {
        dst.write32(0);
        continue;
      }
      dst.write32(count);
      for (uint32 i = 0; i < count; ++i) {
        dst.write32(tm.x08_TagMap[i * 3 + 2]);
        dst.write32(tm.x08_TagMap[i * 3 + 3]);
      }
    }
  }
  void AnimSets() {
    Archive ans;
    SnoFile<GameBalance> gmb("Characters");
    Logger::begin(gmb->x088_Heros.size(), "Dumping characters");
    for (auto& hero : gmb->x088_Heros) {
      Logger::item(hero.x000_Text);
      DumpActorSets(ans, hero.x108_ActorSno);
      DumpActorSets(ans, hero.x10C_ActorSno);
    }
    Logger::end();
    ans.write(File("animsets.wgz", "wb"), true);
  }

  void GenericItems() {
    Logger::begin(3, "Loading assets");
    Logger::item("textures");   Archive tex;// (File("textures.wgz"), false);
    Logger::item("models");     Archive mdl;// (File("models.wgz"), true);
    Logger::item("animations"); Archive ani;// (File("animations.wgz"), true);
    Logger::end();
    //texArchive = &tex;

    json::Value actors;
    json::parse(File("d3gl_actors.js"), actors, json::mJS);
    std::set<uint32> done;
    json::Value items;
    json::Value itemsout;
    json::parse(File("itemtypes.js"), items, json::mJS);
    Logger::begin(items["itemById"].getMap().size(), "Dumping items");
    for (auto& kv : items["itemById"].getMap()) {
      Logger::item(kv.first.c_str());
      auto* item = ItemLibrary::get(kv.first);
      if (!item) continue;
      std::string type = kv.second["type"].getString();
      std::string slot = items["itemTypes"][type]["slot"].getString();
      if (type != "mojo") continue;
      DumpItemActor(mdl, ani, done, item->x108_ActorSno, type == "source" || type == "mojo");
      FillItemInfo(itemsout[item->x000_Text], actors, *item, type, slot);
    }
    Logger::end();

    json::Value genitems;
    json::Value generic;
    json::parse(File("d3gl_items.js"), generic, json::mJS);
    for (auto& kv : generic.getMap()) {
      std::string id = kv.first;
      if (!kv.second.has("type")) continue;
      std::string type = kv.second["type"].getString();
      if (!items["itemTypes"].has(type)) {
        Logger::log("unknown type: %s", type.c_str());
        continue;
      }
      std::string slot = items["itemTypes"][type]["slot"].getString();
      auto* item = ItemLibrary::get(id);
      if (!item || !Actor::name(item->x108_ActorSno)) continue;
      if (type != "mojo") continue;
      auto& dst = genitems[id];
      dst["name"] = Strings::get("Items", id);
      dst["type"] = type;
      dst["promo"] = true;
      FillItemInfo(dst, actors, *item, type, slot);
    }
    json::write(File("extra_actors.js", "w"), actors, json::mJS);
    json::write(File("extra_items.js", "w"), genitems, json::mJS);
    json::write(File("extra_items_orig.js", "w"), itemsout, json::mJS);
    //return;

    Logger::begin(3, "Writing assets");
    //Logger::item("textures");   tex.write(File("textures.wgz", "wb"), false);
    //Logger::item("models");     mdl.write(File("models.wgz", "wb"), true);
    //Logger::item("animations"); ani.write(File("animations.wgz", "wb"), true);
    Logger::end();
    texArchive = nullptr;
  }

  static std::string trim_number(std::string const& src) {
    std::string dst(src);
    while (dst.length() && isdigit((unsigned char)dst.back())) {
      dst.pop_back();
    }
    return dst;
  }

  void AllItems(bool models, bool info, bool load) {
    Archive tex, mdl, ani;
    if (models && load) {
      Logger::begin(3, "Loading assets");
      Logger::item("textures");   tex.load(File("textures.wgz"), false);
      Logger::item("models");     mdl.load(File("models.wgz"), true);
      Logger::item("animations"); ani.load(File("animations.wgz"), true);
      Logger::end();
    }
    texArchive = &tex;
    
    SnoFile<GameBalance> gmb("Characters");
    Logger::begin(gmb->x088_Heros.size(), "Dumping characters");
    for (auto& hero : gmb->x088_Heros) {
      Logger::item(hero.x000_Text);
      DumpActorData(mdl, ani, hero.x108_ActorSno);
      DumpActorData(mdl, ani, hero.x10C_ActorSno);
    }
    Logger::end();
    json::Value items, itemsout, actors;
    if (info && load) json::parse(File("d3gl_actors.js"), actors, json::mJS);
    json::parse(File("itemtypes.js"), items, json::mJS);
    std::set<uint32> done;
    auto stlItems = Strings::list("Items");
    
    // regular items
    Logger::begin(items["itemById"].getMap().size(), "Unique items");
    for (auto& kv : items["itemById"].getMap()) {
      Logger::item(kv.first.c_str());
      auto* item = ItemLibrary::get(kv.first);
      if (!item) {
        Logger::log("unknown item: %s", kv.first.c_str());
        continue;
      }
      std::string type = kv.second["type"].getString();
      std::string slot = items["itemTypes"][type]["slot"].getString();
      if (models) DumpItemActor(mdl, ani, done, item->x108_ActorSno, type == "source" || type == "mojo");
      if (info) {
        json::Value out;
        FillItemInfo(out, actors, *item, type, slot);
        if (out.has("actor") || out.has("armortype")) {
          itemsout[kv.first] = out;
        }
      }
    }
    Logger::end();

    // white items
    Dictionary generics;
    for (auto& kv : items["itemTypes"].getMap()) {
      std::string mask = trim_number(kv.second["generic"].getString());
      generics[mask] = kv.first;
    }
    std::set<uint32> actorsUsed;
    Logger::begin(ItemLibrary::all().size(), "Generic items");
    for (auto& kv : ItemLibrary::all()) {
      Logger::item(kv.first.c_str());
      std::string mask = trim_number(kv.first);
      if (!generics.count(mask)) continue;
      std::string type = generics[mask];
      auto* item = kv.second;
      if (actorsUsed.count(item->x108_ActorSno)) continue;
      actorsUsed.insert(item->x108_ActorSno);

      std::string slot = items["itemTypes"][type]["slot"].getString();
      if (models) DumpItemActor(mdl, ani, done, item->x108_ActorSno, type == "source" || type == "mojo");
      if (info) {
        json::Value out;
        FillItemInfo(out, actors, *item, type, slot);
        if (out.has("actor") || out.has("armortype")) {
          out["type"] = type;
          out["name"] = stlItems[kv.first];
          itemsout[kv.first] = out;
        }
      }
    }
    Logger::end();

    // promo items
    json::Value promo;
    json::parse(File("extraitems.js"), promo, json::mJS);
    Logger::begin(promo.getMap().size());
    for (auto& kv : promo.getMap()) {
      Logger::item(kv.first.c_str());
      auto* item = ItemLibrary::get(kv.first);
      if (!item) {
        Logger::log("unknown item: %s", kv.first.c_str());
        continue;
      }
      std::string type = GameAffixes::getItemType(item->x10C_ItemTypesGameBalanceId);
      //std::string type = kv.second["type"].getString();
      std::string slot = items["itemTypes"][type]["slot"].getString();
      if (models) DumpItemActor(mdl, ani, done, item->x108_ActorSno, type == "source" || type == "mojo");
      if (info) {
        auto& out = itemsout[kv.first];
        FillItemInfo(out, actors, *item, type, slot);
        out["type"] = type;
        out["name"] = stlItems[kv.first];
        out["promo"] = true;
      }
    }
    Logger::end();

    for (auto it = actors.begin(); it != actors.end(); ++it) {
      uint32 id = atoi(it.key().c_str());
      SnoFile<Actor> actor(Actor::name(id));
      if (!actor) continue;
      uint32 physics = actor->x2B4_PhysicsSno;
      if (Physics::name(physics)) {
        (*it)["physics"] = physics;
      }
    }

    if (info) {
      json::write(File("webgl_actors.js", "w"), actors, json::mJS);
      json::write(File("webgl_items.js", "w"), itemsout, json::mJS);
    }

    if (models) {
      Logger::begin(3, "Writing assets");
      Logger::item("textures");   tex.write(File("textures.wgz", "wb"), false);
      Logger::item("models");     mdl.write(File("models.wgz", "wb"), true);
      Logger::item("animations"); ani.write(File("animations.wgz", "wb"), true);
      Logger::end();
      texArchive = nullptr;
    }
  }

  void AddPhysics() {
    json::Value actors, phys;
    json::Value items;
    json::parse(File("d3gl_actors.js"), actors, json::mJS);
    json::parse(File("d3gl_items.js"), items, json::mJS);
    std::map<uint32, std::string> actor2item;
    for (auto& kv : items.getMap()) {
      auto& actor = kv.second["actor"];
      std::string name = Strings::get("Items", kv.first);
      if (actor.type() == json::Value::tObject) {
        for (auto& sub : actor) {
          actor2item[sub.getInteger()] = name;
        }
      } else {
        actor2item[actor.getInteger()] = name;
      }
    }
    for (auto it = actors.begin(); it != actors.end(); ++it) {
      uint32 id = atoi(it.key().c_str());
      SnoFile<Actor> actor(Actor::name(id));
      if (!actor) continue;
      uint32 physics = actor->x2B4_PhysicsSno;
      if (Physics::name(physics)) {
        (*it)["physics"] = physics;
        if (actor2item.count(id)) {
          phys[actor2item[id]] = Physics::name(physics);
        }
      }
    }
    json::write(File("d3gl_actors.js", "w"), actors, json::mJS);
    json::write(File("d3gl_physics.js", "w"), phys, json::mJS);
  }

  struct ObjMaterial {
    std::vector<uint8> tex;
    char mime[16];
    float diff[4];
    float spec[3];

    ObjMaterial() {
      memset(mime, 0, sizeof mime);
      memset(diff, 0, sizeof diff);
      memset(spec, 0, sizeof spec);
      diff[3] = 1.0f;
    }
  };
  typedef std::tuple<int, int, int> VertexIndex;
  struct ObjVertexNormal {
    int8 normal[4];
    ObjVertexNormal() {}
    ObjVertexNormal(float x, float y, float z) {
      float d = std::sqrt(x * x + y * y + z * z);
      normal[0] = static_cast<int8>(x / d * 127.0f);
      normal[1] = static_cast<int8>(y / d * 127.0f);
      normal[2] = static_cast<int8>(z / d * 127.0f);
      normal[3] = 0;
    }
    Vector vec() const {
      return Vector(normal[0] / 127.0f, normal[1] / 127.0f, normal[2] / 127.0f);
    }
  };
  struct ObjVertexTex {
    int16 texcoord[2];
    ObjVertexTex() {}
    ObjVertexTex(float u, float v) {
      texcoord[0] = static_cast<int16>(u * 512.0f);
      texcoord[1] = static_cast<int16>(v * 512.0f);
    }
  };
  struct ObjVertex {
    Vector v;
    ObjVertexNormal n;
    ObjVertexTex t;
    ObjVertex() {}
    ObjVertex(Vector const& v, ObjVertexTex const& t, ObjVertexNormal const& n)
      : v(v)
      , t(t)
      , n(n)
    {}
  };

  std::vector<uint8> read_file(std::string const& path) {
    File file(path);
    std::vector<uint8> vec(file.size());
    file.read(vec.data(), vec.size());
    return vec;
  }

  void parseObj(char const* path, char const* dst) {
    std::map<std::string, int> mat_index;
    std::vector<ObjMaterial> mat_list;


    std::vector<Vector> v_list;
    std::vector<ObjVertexTex> t_list;
    std::vector<ObjVertexNormal> n_list;


    struct Object {
      uint32 mat;
      std::map<VertexIndex, uint16> vertex_index;
      std::vector<ObjVertex> vertex_list;
      std::vector<uint16> index_list;
      std::vector<uint16> face_list;
      std::vector<uint32> face_sizes;
    };

    ObjVertexTex t0(0, 0);
    ObjVertexNormal n0(0, 0, 0);

    std::vector<Object> objects;

    MemoryFile output;

    std::string root = path::path(path);
    for (std::string line : File(path)) {
      trim(line);
      if (line.empty() || line[0] == '#') continue;

      size_t space = line.find(' ');
      std::string cmd = line.substr(0, space);
      line = trim(line.substr(space));

      if (cmd == "mtllib") {
        for (std::string subline : File(root / line)) {
          trim(subline);
          if (subline.empty() || subline[0] == '#') continue;

          size_t space = subline.find(' ');
          std::string subcmd = subline.substr(0, space);
          subline = trim(subline.substr(space));

          if (subcmd == "newmtl") {
            mat_index[subline] = mat_list.size();
            mat_list.emplace_back();
          } else if (subcmd == "Kd") {
            auto parts = split(subline);
            mat_list.back().diff[0] = std::stof(parts[0]);
            mat_list.back().diff[1] = std::stof(parts[1]);
            mat_list.back().diff[2] = std::stof(parts[2]);
          } else if (subcmd == "Ks") {
            auto parts = split(subline);
            mat_list.back().spec[0] = std::stof(parts[0]);
            mat_list.back().spec[1] = std::stof(parts[1]);
            mat_list.back().spec[2] = std::stof(parts[2]);
          } else if (subcmd == "d") {
            mat_list.back().diff[3] = std::stof(subline);
          } else if (subcmd == "map_Kd") {
            mat_list.back().tex = read_file(root / subline);
            std::string ext = strlower(path::ext(subline));
            if (ext == ".jpg") {
              strcpy(mat_list.back().mime, "image/jpeg");
            } else if (ext == ".png") {
              strcpy(mat_list.back().mime, "image/png");
            }
          }
        }
      } else if (cmd == "usemtl") {
        objects.emplace_back();
        objects.back().mat = mat_index[line];
      } else if (cmd == "v") {
        auto parts = split(line);
        v_list.emplace_back(std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]));
        v_list.back() *= 1e-6f;
      } else if (cmd == "vt") {
        auto parts = split(line);
        t_list.emplace_back(std::stof(parts[0]), std::stof(parts[1]));
      } else if (cmd == "vn") {
        auto parts = split(line);
        n_list.emplace_back(std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]));
      } else if (cmd == "f") {
        auto parts = split(line);
        std::vector<uint16> indices;

        Object& obj = objects.back();
        for (auto const& p : parts) {
          auto cp = split(p, '/');
          int v = std::stoi(cp[0]);
          int t = (cp.size() < 2 || cp[1].empty() ? 0 : std::stoi(cp[1]));
          int n = (cp.size() < 3 ? 0 : std::stoi(cp[2]));
          VertexIndex idx(v, t, n);
          if (!objects.back().vertex_index.count(idx)) {
            obj.vertex_index[idx] = obj.vertex_list.size();
            obj.vertex_list.emplace_back(v_list[v - 1], t ? t_list[t - 1] : t0, n ? n_list[n - 1] : n0);
          }
          indices.push_back(obj.vertex_index[idx]);
        }

        std::vector<Vector> vn;
        for (size_t i = 0; i < indices.size(); ++i) {
          Vector c1 = obj.vertex_list[indices[i]].v;
          Vector c0 = obj.vertex_list[indices[i == 0 ? indices.size() - 1 : i - 1]].v;
          Vector c2 = obj.vertex_list[indices[(i + 1) % indices.size()]].v;
          vn.push_back((c1 - c0) ^ (c2 - c1));
        }
        Vector n = obj.vertex_list[indices[0]].n.vec();
        if (n.length2() < 1e-4) {
          n = Vector(0, 0, 0);
          for (auto& cn : vn) {
            if ((cn & n) > 0) {
              n += cn;
            } else {
              n -= cn;
            }
          }
          n.normalize();
        }

        bool hp = false, hn = false;
        for (auto& cn : vn) {
          float dt = (cn & n);
          if (dt > 1e-4) {
            hp = true;
          } else if (dt < -1e-4) {
            hn = true;
          }
        }

        if (hp && hn) {
          if (vn.size() == 25) {
            int asdf = 0;
          }
          n = Vector(0, 0, 0);
          for (size_t i = 1; i < indices.size() - 1; ++i) {
            Vector c0 = obj.vertex_list[indices[0]].v;
            Vector c1 = obj.vertex_list[indices[i]].v;
            Vector c2 = obj.vertex_list[indices[i + 1]].v;
            n += (c1 - c0) ^ (c2 - c0);
          }
          n.normalize();
          while (indices.size() > 3) {
            size_t ss = indices.size();
            for (size_t i = 0; i < indices.size(); ++i) {
              uint16 i0 = (i == 0 ? indices.size() - 1 : i - 1), i2 = (i + 1) % indices.size();
              Vector c1 = obj.vertex_list[indices[i]].v;
              Vector c0 = obj.vertex_list[indices[i0]].v;
              Vector c2 = obj.vertex_list[indices[i2]].v;
              Vector cn = (c1 - c0) ^ (c2 - c1);
              float vd = (cn & n);
              if (vd > -1e-8 && vd < 1e-8) {
                indices.erase(indices.begin() + i);
                break;
              }
              if (vd < 0) continue;
              bool ok = true;
              for (size_t j = 0; j < indices.size(); ++j) {
                if (j == i || j == i0 || j == i2) continue;
                Vector v = obj.vertex_list[indices[j]].v;
                Vector v0 = (v - c1) ^ (c2 - c1);
                Vector v1 = (v - c2) ^ (c0 - c2);
                Vector v2 = (v - c0) ^ (c1 - c0);
                if ((v0 & n) < -1e-4 && (v1 & n) < -1e-4 && (v2 & n) < -1e-4) {
                  ok = false;
                  break;
                }
              }
              if (ok) {
                obj.index_list.push_back(indices[i0]);
                obj.index_list.push_back(indices[i]);
                obj.index_list.push_back(indices[i2]);
                obj.face_list.push_back(indices[i0]);
                obj.face_list.push_back(indices[i]);
                obj.face_list.push_back(indices[i2]);
                obj.face_sizes.push_back(3);
                indices.erase(indices.begin() + i);
                break;
              }
            }
            if (indices.size() == ss) {
              n = Vector(0, 0, 0);
              for (size_t i = 1; i < indices.size() - 1; ++i) {
                Vector c0 = obj.vertex_list[indices[0]].v;
                Vector c1 = obj.vertex_list[indices[i]].v;
                Vector c2 = obj.vertex_list[indices[i + 1]].v;
                n += (c1 - c0) ^ (c2 - c0);
              }
              n.normalize();
              int asdf = 0;
            }
          }
          obj.index_list.push_back(indices[0]);
          obj.index_list.push_back(indices[1]);
          obj.index_list.push_back(indices[2]);
          obj.face_list.push_back(indices[0]);
          obj.face_list.push_back(indices[1]);
          obj.face_list.push_back(indices[2]);
          obj.face_sizes.push_back(3);
        } else {
          if (hn && !hp) {
            std::reverse(indices.begin(), indices.end());
          }
          obj.face_list.push_back(indices[0]);
          for (size_t i = 1; i < indices.size() - 1; ++i) {
            obj.index_list.push_back(indices[0]);
            obj.index_list.push_back(indices[i]);
            obj.index_list.push_back(indices[i + 1]);
            obj.face_list.push_back(indices[i]);
          }
          obj.face_list.push_back(indices.back());
          obj.face_sizes.push_back(indices.size());
        }
      } else if (cmd == "g") {
      } else {
        int adsf = 0;
      }
    }

    uint32 mat_offset = 32;
    uint32 tex_offset = mat_offset + mat_list.size() * 52;
    uint32 model_offset = tex_offset;
    for (auto const& mat : mat_list) {
      model_offset += mat.tex.size();
      model_offset = (model_offset + 3) & (~3);
    }
    uint32 vertex_offset = model_offset + objects.size() * 36;

    auto vsh = read_file(root / "shader.vsh");
    auto psh = read_file(root / "shader.psh");

    uint32 vsh_offset = vertex_offset;
    for (auto const& mdl : objects) {
      vsh_offset += mdl.vertex_list.size() * sizeof(ObjVertex);
      vsh_offset += mdl.index_list.size() * 2;
      vsh_offset = (vsh_offset + 3) & (~3);
      vsh_offset += mdl.face_sizes.size() * 4;
      vsh_offset += mdl.face_list.size() * 2;
      vsh_offset = (vsh_offset + 3) & (~3);
    }
    uint32 psh_offset = vsh_offset + vsh.size();

    output.write32(mat_list.size());
    output.write32(mat_offset);
    output.write32(objects.size());
    output.write32(model_offset);
    output.write32(vsh_offset);
    output.write32(vsh.size());
    output.write32(psh_offset);
    output.write32(psh.size());
    for (auto const& mat : mat_list) {
      output.write(mat.mime, sizeof mat.mime);
      output.write32(mat.tex.size() ? tex_offset : 0);
      output.write32(mat.tex.size());
      tex_offset += mat.tex.size();
      tex_offset = (tex_offset + 3) & (~3);
      output.write(mat.diff, sizeof mat.diff);
      output.write(mat.spec, sizeof mat.spec);
    }
    for (auto const& mat : mat_list) {
      if (mat.tex.size()) {
        output.write(&mat.tex[0], mat.tex.size());
        size_t size = mat.tex.size();
        while (size & 3) {
          output.write8(0);
          size++;
        }
      }
    }
    for (auto const& mdl : objects) {
      output.write32(mdl.mat);
      output.write32(mdl.vertex_list.size());
      output.write32(vertex_offset);
      vertex_offset += mdl.vertex_list.size() * sizeof(ObjVertex);
      output.write32(mdl.index_list.size());
      output.write32(vertex_offset);
      vertex_offset += mdl.index_list.size() * 2;
      vertex_offset = (vertex_offset + 3) & (~3);
      output.write32(mdl.face_sizes.size());
      output.write32(mdl.face_list.size());
      output.write32(vertex_offset);
      vertex_offset += mdl.face_sizes.size() * 4;
      output.write32(vertex_offset);
      vertex_offset += mdl.face_list.size() * 2;
      vertex_offset = (vertex_offset + 3) & (~3);
    }
    for (auto const& mdl : objects) {
      for (auto const& v : mdl.vertex_list) {
        output.write(v);
      }
      for (uint16 i : mdl.index_list) {
        output.write16(i);
      }
      if (mdl.index_list.size() & 1) {
        output.write16(0);
      }
      for (uint16 i : mdl.face_sizes) {
        output.write32(i);
      }
      for (uint16 i : mdl.face_list) {
        output.write16(i);
      }
      if (mdl.face_list.size() & 1) {
        output.write16(0);
      }
    }
    output.write(vsh.data(), vsh.size());
    output.write(psh.data(), psh.size());
    uint32 outSize = output.size();
    std::vector<uint8> odata(outSize);
    gzencode(output.data(), output.size(), odata.data(), &outSize);
    odata.resize(outSize);
    File cout(dst, "wb");
    cout.write(odata.data(), odata.size());
  }

}
