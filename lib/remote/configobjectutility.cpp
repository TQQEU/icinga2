/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/configobjectutility.hpp"
#include "remote/configpackageutility.hpp"
#include "remote/apilistener.hpp"
#include "config/configcompiler.hpp"
#include "config/configitem.hpp"
#include "base/atomic-file.hpp"
#include "base/configwriter.hpp"
#include "base/exception.hpp"
#include "base/dependencygraph.hpp"
#include "base/tlsutility.hpp"
#include "base/utility.hpp"
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <utility>

using namespace icinga;

String ConfigObjectUtility::GetConfigDir()
{
	String prefix = ConfigPackageUtility::GetPackageDir() + "/_api/";
	String activeStage = ConfigPackageUtility::GetActiveStage("_api");

	if (activeStage.IsEmpty())
		RepairPackage("_api");

	return prefix + activeStage;
}

String ConfigObjectUtility::ComputeNewObjectConfigPath(const Type::Ptr& type, const String& fullName)
{
	String typeDir = type->GetPluralName();
	boost::algorithm::to_lower(typeDir);

	/* This may throw an exception the caller above must handle. */
	String prefix = GetConfigDir() + "/conf.d/" + type->GetPluralName().ToLower() + "/";

	String escapedName = EscapeName(fullName);

	String longPath = prefix + escapedName + ".conf";

	/*
	 * The long path may cause trouble due to exceeding the allowed filename length of the filesystem. Therefore, the
	 * preferred solution would be to use the truncated and hashed version as returned at the end of this function.
	 * However, for compatibility reasons, we have to keep the old long version in some cases. Notably, this could lead
	 * to the creation of objects that can't be synced to child nodes if they are running an older version. Thus, for
	 * now, the fix is only enabled for comments and downtimes, as these are the object types for which the issue is
	 * most likely triggered but can't be worked around easily (you'd have to rename the host and/or service in order to
	 * be able to schedule a downtime or add an acknowledgement, which is not feasible) and the impact of not syncing
	 * these objects through the whole cluster is limited. For other object types, we currently prefer to fail the
	 * creation early so that configuration inconsistencies throughout the cluster are avoided.
	 *
	 * TODO: Remove this in v2.16 and truncate all.
	 */
	if (type->GetName() != "Comment" && type->GetName() != "Downtime") {
		return longPath;
	}

	/* Maximum length 80 bytes object name + 3 bytes "..." + 40 bytes SHA1 (hex-encoded) */
	return prefix + Utility::TruncateUsingHash<80+3+40>(escapedName) + ".conf";
}

String ConfigObjectUtility::GetExistingObjectConfigPath(const ConfigObject::Ptr& object)
{
	return object->GetDebugInfo().Path;
}

void ConfigObjectUtility::RepairPackage(const String& package)
{
	/* Try to fix the active stage, whenever we find a directory in there.
	 * This automatically heals packages < 2.11 which remained broken.
	 */
	String dir = ConfigPackageUtility::GetPackageDir() + "/" + package + "/";

	namespace fs = boost::filesystem;

	/* Use iterators to workaround VS builds on Windows. */
	fs::path path(dir.Begin(), dir.End());

	fs::recursive_directory_iterator end;

	String foundActiveStage;

	for (fs::recursive_directory_iterator it(path); it != end; ++it) {
		boost::system::error_code ec;

		const fs::path d = *it;
		if (fs::is_directory(d, ec)) {
			/* Extract the relative directory name. */
			foundActiveStage = d.stem().string();

			break; // Use the first found directory.
		}
	}

	if (!foundActiveStage.IsEmpty()) {
		Log(LogInformation, "ConfigObjectUtility")
			<< "Repairing config package '" << package << "' with stage '" << foundActiveStage << "'.";

		ConfigPackageUtility::ActivateStage(package, foundActiveStage);
	} else {
		BOOST_THROW_EXCEPTION(std::invalid_argument("Cannot repair package '" + package + "', please check the troubleshooting docs."));
	}
}

void ConfigObjectUtility::CreateStorage()
{
	std::unique_lock<std::mutex> lock(ConfigPackageUtility::GetStaticPackageMutex());

	/* For now, we only use _api as our creation target. */
	String package = "_api";

	if (!ConfigPackageUtility::PackageExists(package)) {
		Log(LogNotice, "ConfigObjectUtility")
			<< "Package " << package << " doesn't exist yet, creating it.";

		ConfigPackageUtility::CreatePackage(package);

		String stage = ConfigPackageUtility::CreateStage(package);
		ConfigPackageUtility::ActivateStage(package, stage);
	}
}

String ConfigObjectUtility::EscapeName(const String& name)
{
	return Utility::EscapeString(name, "<>:\"/\\|?*", true);
}

String ConfigObjectUtility::CreateObjectConfig(const Type::Ptr& type, const String& fullName,
	bool ignoreOnError, const Array::Ptr& templates, const Dictionary::Ptr& attrs)
{
	auto *nc = dynamic_cast<NameComposer *>(type.get());
	Dictionary::Ptr nameParts;
	String name;

	if (nc) {
		nameParts = nc->ParseName(fullName);
		name = nameParts->Get("name");
	} else
		name = fullName;

	Dictionary::Ptr allAttrs = new Dictionary();

	if (attrs) {
		attrs->CopyTo(allAttrs);

		ObjectLock olock(attrs);
		for (const Dictionary::Pair& kv : attrs) {
			int fid = type->GetFieldId(kv.first.SubStr(0, kv.first.FindFirstOf(".")));

			if (fid < 0)
				BOOST_THROW_EXCEPTION(ScriptError("Invalid attribute specified: " + kv.first));

			Field field = type->GetFieldInfo(fid);

			if (!(field.Attributes & FAConfig) || kv.first == "name")
				BOOST_THROW_EXCEPTION(ScriptError("Attribute is marked for internal use only and may not be set: " + kv.first));
		}
	}

	if (nameParts)
		nameParts->CopyTo(allAttrs);

	allAttrs->Remove("name");

	/* update the version for config sync */
	allAttrs->Set("version", Utility::GetTime());

	std::ostringstream config;
	ConfigWriter::EmitConfigItem(config, type->GetName(), name, false, ignoreOnError, templates, allAttrs);
	ConfigWriter::EmitRaw(config, "\n");

	return config.str();
}

bool ConfigObjectUtility::CreateObject(const Type::Ptr& type, const String& fullName,
	const String& config, const Array::Ptr& errors, const Array::Ptr& diagnosticInformation, const Value& cookie)
{
	CreateStorage();

	{
		auto configType (dynamic_cast<ConfigType*>(type.get()));

		if (configType && configType->GetObject(fullName)) {
			errors->Add("Object '" + fullName + "' already exists.");
			return false;
		}
	}

	String path;

	try {
		path = ComputeNewObjectConfigPath(type, fullName);
	} catch (const std::exception& ex) {
		errors->Add("Config package broken: " + DiagnosticInformation(ex, false));
		return false;
	}

	// AtomicFile doesn't create not yet existing directories, so we have to do it by ourselves.
	Utility::MkDirP(Utility::DirName(path), 0700);

	// Using AtomicFile guarantees that two different threads simultaneously creating and loading the same
	// configuration file do not interfere with each other, as the configuration is stored in a unique temp file.
	// When one thread fails to pass object validation, it only deletes its temporary file and does not affect
	// the other thread in any way.
	AtomicFile fp(path, 0644);
	fp << config;
	// Flush the output buffer to catch any errors ASAP and handle them accordingly!
	// Note: AtomicFile places these configs in a temp file and will be automatically
	//       discarded when it is not committed before going out of scope.
	fp.flush();

	std::unique_ptr<Expression> expr = ConfigCompiler::CompileText(path, config, String(), "_api");

	try {
		ActivationScope ascope;

		ScriptFrame frame(true);
		expr->Evaluate(frame);
		expr.reset();

		WorkQueue upq;
		upq.SetName("ConfigObjectUtility::CreateObject");

		std::vector<ConfigItem::Ptr> newItems;

		/*
		 * Disable logging for object creation, but do so ourselves later on.
		 * Duplicate the error handling for better logging and debugging here.
		 */
		if (!ConfigItem::CommitItems(ascope.GetContext(), upq, newItems, true)) {
			if (errors) {
				Log(LogNotice, "ConfigObjectUtility")
					<< "Failed to commit config item '" << fullName << "'.";

				for (const boost::exception_ptr& ex : upq.GetExceptions()) {
					errors->Add(DiagnosticInformation(ex, false));

					if (diagnosticInformation)
						diagnosticInformation->Add(DiagnosticInformation(ex));
				}
			}

			return false;
		}

		/*
		 * Activate the config object.
		 * uq, items, runtimeCreated, silent, withModAttrs, cookie
		 * IMPORTANT: Forward the cookie aka origin in order to prevent sync loops in the same zone!
		 */
		if (!ConfigItem::ActivateItems(newItems, true, false, false, cookie)) {
			if (errors) {
				Log(LogNotice, "ConfigObjectUtility")
					<< "Failed to activate config object '" << fullName << "'.";

				for (const boost::exception_ptr& ex : upq.GetExceptions()) {
					errors->Add(DiagnosticInformation(ex, false));

					if (diagnosticInformation)
						diagnosticInformation->Add(DiagnosticInformation(ex));
				}
			}

			return false;
		}

		/* if (type != Comment::TypeInstance && type != Downtime::TypeInstance)
		 * Does not work since this would require libicinga, which has a dependency on libremote
		 * Would work if these libs were static.
		 */
		if (type->GetName() != "Comment" && type->GetName() != "Downtime")
			ApiListener::UpdateObjectAuthority();

		// At this stage we should have a config object already. If not, it was ignored before.
		auto *ctype = dynamic_cast<ConfigType *>(type.get());
		ConfigObject::Ptr obj = ctype->GetObject(fullName);

		if (obj) {
			// Object has surpassed the compiling/validation processes, we can safely commit the file!
			fp.Commit();

			Log(LogInformation, "ConfigObjectUtility")
				<< "Created and activated object '" << fullName << "' of type '" << type->GetName() << "'.";
		} else {
			Log(LogNotice, "ConfigObjectUtility")
				<< "Object '" << fullName << "' was not created but ignored due to errors.";
		}

	} catch (const std::exception& ex) {
		if (errors)
			errors->Add(DiagnosticInformation(ex, false));

		if (diagnosticInformation)
			diagnosticInformation->Add(DiagnosticInformation(ex));

		return false;
	}

	return true;
}

bool ConfigObjectUtility::DeleteObjectHelper(const ConfigObject::Ptr& object, bool cascade,
	const Array::Ptr& errors, const Array::Ptr& diagnosticInformation, const Value& cookie)
{
	std::vector<Object::Ptr> parents = DependencyGraph::GetParents(object);

	Type::Ptr type = object->GetReflectionType();

	String name = object->GetName();

	if (!parents.empty() && !cascade) {
		if (errors) {
			errors->Add("Object '" + name + "' of type '" + type->GetName() +
				"' cannot be deleted because other objects depend on it. "
				"Use cascading delete to delete it anyway.");
		}

		return false;
	}

	for (const Object::Ptr& pobj : parents) {
		ConfigObject::Ptr parentObj = dynamic_pointer_cast<ConfigObject>(pobj);

		if (!parentObj)
			continue;

		DeleteObjectHelper(parentObj, cascade, errors, diagnosticInformation, cookie);
	}

	ConfigItem::Ptr item = ConfigItem::GetByTypeAndName(type, name);

	try {
		/* mark this object for cluster delete event */
		object->SetExtension("ConfigObjectDeleted", true);

		/*
		 * Trigger deactivation signal for DB IDO and runtime object delections.
		 * IMPORTANT: Specify the cookie aka origin in order to prevent sync loops
		 * in the same zone!
		 */
		object->Deactivate(true, cookie);

		if (item)
			item->Unregister();
		else
			object->Unregister();

	} catch (const std::exception& ex) {
		if (errors)
			errors->Add(DiagnosticInformation(ex, false));

		if (diagnosticInformation)
			diagnosticInformation->Add(DiagnosticInformation(ex));

		return false;
	}

	if (object->GetPackage() == "_api") {
		Utility::Remove(GetExistingObjectConfigPath(object));
	}

	Log(LogInformation, "ConfigObjectUtility")
		<< "Deleted object '" << name << "' of type '" << type->GetName() << "'.";

	return true;
}

bool ConfigObjectUtility::DeleteObject(const ConfigObject::Ptr& object, bool cascade, const Array::Ptr& errors,
	const Array::Ptr& diagnosticInformation, const Value& cookie)
{
	if (object->GetPackage() != "_api") {
		if (errors)
			errors->Add("Object cannot be deleted because it was not created using the API.");

		return false;
	}

	return DeleteObjectHelper(object, cascade, errors, diagnosticInformation, cookie);
}
