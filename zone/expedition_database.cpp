/**
 * EQEmulator: Everquest Server Emulator
 * Copyright (C) 2001-2020 EQEmulator Development Team (https://github.com/EQEmu/Server)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY except by those people which sell it, which
 * are required to give you total support for your newly bought product;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "expedition_database.h"
#include "expedition.h"
#include "expedition_lockout_timer.h"
#include "zonedb.h"
#include "../common/database.h"
#include "../common/string_util.h"
#include <fmt/core.h>

uint32_t ExpeditionDatabase::InsertExpedition(
	const std::string& uuid, uint32_t instance_id, const std::string& expedition_name,
	uint32_t leader_id, uint32_t min_players, uint32_t max_players)
{
	LogExpeditionsDetail(
		"Inserting new expedition [{}] leader [{}] uuid [{}]", expedition_name, leader_id, uuid
	);

	std::string query = fmt::format(SQL(
		INSERT INTO expedition_details
			(uuid, instance_id, expedition_name, leader_id, min_players, max_players)
		VALUES
			('{}', {}, '{}', {}, {}, {});
	), uuid, instance_id, EscapeString(expedition_name), leader_id, min_players, max_players);

	auto results = database.QueryDatabase(query);
	if (!results.Success())
	{
		LogExpeditions("Failed to obtain an expedition id for [{}]", expedition_name);
		return 0;
	}

	return results.LastInsertedID();
}

std::string ExpeditionDatabase::LoadExpeditionsSelectQuery()
{
	return std::string(SQL(
		SELECT
			expedition_details.id,
			expedition_details.uuid,
			expedition_details.instance_id,
			expedition_details.expedition_name,
			expedition_details.leader_id,
			expedition_details.min_players,
			expedition_details.max_players,
			expedition_details.add_replay_on_join,
			expedition_details.is_locked,
			character_data.name leader_name,
			expedition_members.character_id,
			member_data.name
		FROM expedition_details
			INNER JOIN character_data ON expedition_details.leader_id = character_data.id
			INNER JOIN expedition_members ON expedition_details.id = expedition_members.expedition_id
			INNER JOIN character_data member_data ON expedition_members.character_id = member_data.id
	));
}

MySQLRequestResult ExpeditionDatabase::LoadExpedition(uint32_t expedition_id)
{
	LogExpeditionsDetail("Loading expedition [{}]", expedition_id);

	std::string query = fmt::format(SQL(
		{} WHERE expedition_details.id = {};
	), LoadExpeditionsSelectQuery(), expedition_id);

	return database.QueryDatabase(query);
}

MySQLRequestResult ExpeditionDatabase::LoadAllExpeditions()
{
	LogExpeditionsDetail("Loading all expeditions from database");

	std::string query = fmt::format(SQL(
		{} ORDER BY expedition_details.id;
	), LoadExpeditionsSelectQuery());

	return database.QueryDatabase(query);
}

std::vector<ExpeditionLockoutTimer> ExpeditionDatabase::LoadCharacterLockouts(uint32_t character_id)
{
	LogExpeditionsDetail("Loading character [{}] lockouts", character_id);

	std::vector<ExpeditionLockoutTimer> lockouts;

	auto query = fmt::format(SQL(
		SELECT
			from_expedition_uuid,
			expedition_name,
			event_name,
			UNIX_TIMESTAMP(expire_time),
			duration
		FROM expedition_character_lockouts
		WHERE character_id = {} AND is_pending = FALSE AND expire_time > NOW();
	), character_id);

	auto results = database.QueryDatabase(query);
	if (results.Success())
	{
		for (auto row = results.begin(); row != results.end(); ++row)
		{
			lockouts.emplace_back(
				row[0],                                             // expedition_uuid
				row[1],                                             // expedition_name
				row[2],                                             // event_name
				strtoull(row[3], nullptr, 10),                      // expire_time
				static_cast<uint32_t>(strtoul(row[4], nullptr, 10)) // duration
			);
		}
	}

	return lockouts;
}

std::vector<ExpeditionLockoutTimer> ExpeditionDatabase::LoadCharacterLockouts(
	uint32_t character_id, const std::string& expedition_name)
{
	LogExpeditionsDetail("Loading character [{}] lockouts for [{}]", character_id, expedition_name);

	std::vector<ExpeditionLockoutTimer> lockouts;

	auto query = fmt::format(SQL(
		SELECT
			from_expedition_uuid,
			event_name,
			UNIX_TIMESTAMP(expire_time),
			duration
		FROM expedition_character_lockouts
		WHERE
			character_id = {}
			AND is_pending = FALSE
			AND expire_time > NOW()
			AND expedition_name = '{}';
	), character_id, EscapeString(expedition_name));

	auto results = database.QueryDatabase(query);
	if (results.Success())
	{
		for (auto row = results.begin(); row != results.end(); ++row)
		{
			lockouts.emplace_back(
				row[0],                                             // expedition_uuid
				expedition_name,
				row[1],                                             // event_name
				strtoull(row[2], nullptr, 10),                      // expire_time
				static_cast<uint32_t>(strtoul(row[3], nullptr, 10)) // duration
			);
		}
	}

	return lockouts;
}

std::unordered_map<uint32_t, std::unordered_map<std::string, ExpeditionLockoutTimer>>
ExpeditionDatabase::LoadMultipleExpeditionLockouts(
	const std::vector<uint32_t>& expedition_ids)
{
	LogExpeditionsDetail("Loading internal lockouts for [{}] expeditions", expedition_ids.size());

	std::string in_expedition_ids_query;
	for (const auto& expedition_id : expedition_ids)
	{
		fmt::format_to(std::back_inserter(in_expedition_ids_query), "{},", expedition_id);
	}

	// these are loaded into the same container type expeditions use to store lockouts
	std::unordered_map<uint32_t, std::unordered_map<std::string, ExpeditionLockoutTimer>> lockouts;

	if (!in_expedition_ids_query.empty())
	{
		in_expedition_ids_query.pop_back(); // trailing comma

		std::string query = fmt::format(SQL(
			SELECT
				expedition_lockouts.expedition_id,
				expedition_lockouts.from_expedition_uuid,
				expedition_details.expedition_name,
				expedition_lockouts.event_name,
				UNIX_TIMESTAMP(expedition_lockouts.expire_time),
				expedition_lockouts.duration
			FROM expedition_lockouts
				INNER JOIN expedition_details ON expedition_lockouts.expedition_id = expedition_details.id
			WHERE expedition_id IN ({})
			ORDER BY expedition_id;
		), in_expedition_ids_query);

		auto results = database.QueryDatabase(query);

		if (results.Success())
		{
			for (auto row = results.begin(); row != results.end(); ++row)
			{
				auto expedition_id = strtoul(row[0], nullptr, 10);
				lockouts[expedition_id].emplace(row[3], ExpeditionLockoutTimer{
					row[1],                                               // expedition_uuid
					row[2],                                               // expedition_name
					row[3],                                               // event_name
					strtoull(row[4], nullptr, 10),                        // expire_time
					static_cast<uint32_t>(strtoul(row[5], nullptr, 10))   // original duration
				});
			}
		}
	}

	return lockouts;
}

MySQLRequestResult ExpeditionDatabase::LoadMembersForCreateRequest(
	const std::vector<std::string>& character_names, const std::string& expedition_name)
{
	LogExpeditionsDetail(
		"Loading data of [{}] characters for [{}] request", character_names.size(), expedition_name
	);

	std::string in_character_names_query;
	for (const auto& character_name : character_names)
	{
		fmt::format_to(std::back_inserter(in_character_names_query), "'{}',", character_name);
	}

	MySQLRequestResult results;

	if (!in_character_names_query.empty())
	{
		in_character_names_query.pop_back(); // trailing comma

		// for create validation, loads each character's lockouts and possible current expedition
		auto query = fmt::format(SQL(
			SELECT
				character_data.id,
				character_data.name,
				member.expedition_id,
				lockout.from_expedition_uuid,
				UNIX_TIMESTAMP(lockout.expire_time),
				lockout.duration,
				lockout.event_name
			FROM character_data
				LEFT JOIN expedition_character_lockouts lockout
					ON character_data.id = lockout.character_id
					AND lockout.is_pending = FALSE
					AND lockout.expire_time > NOW()
					AND lockout.expedition_name = '{}'
				LEFT JOIN expedition_members member ON character_data.id = member.character_id
			WHERE character_data.name IN ({})
			ORDER BY character_data.id;
		), EscapeString(expedition_name), in_character_names_query);

		results = database.QueryDatabase(query);
	}

	return results;
}

void ExpeditionDatabase::DeleteAllCharacterLockouts(uint32_t character_id)
{
	LogExpeditionsDetail("Deleting all character [{}] lockouts", character_id);

	if (character_id != 0)
	{
		std::string query = fmt::format(SQL(
			DELETE FROM expedition_character_lockouts
			WHERE character_id = {};
		), character_id);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::DeleteAllCharacterLockouts(
	uint32_t character_id, const std::string& expedition_name)
{
	LogExpeditionsDetail("Deleting all character [{}] lockouts for [{}]", character_id, expedition_name);

	if (character_id != 0 && !expedition_name.empty())
	{
		std::string query = fmt::format(SQL(
			DELETE FROM expedition_character_lockouts
			WHERE character_id = {} AND expedition_name = '{}';
		), character_id, EscapeString(expedition_name));

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::DeleteCharacterLockout(
	uint32_t character_id, const std::string& expedition_name, const std::string& event_name)
{
	LogExpeditionsDetail(
		"Deleting character [{}] lockout: [{}]:[{}]", character_id, expedition_name, event_name
	);

	auto query = fmt::format(SQL(
		DELETE FROM expedition_character_lockouts
		WHERE
			character_id = {}
			AND is_pending = FALSE
			AND expedition_name = '{}'
			AND event_name = '{}';
	), character_id, EscapeString(expedition_name), EscapeString(event_name));

	database.QueryDatabase(query);
}

void ExpeditionDatabase::DeleteMembersLockout(
	const std::vector<ExpeditionMember>& members,
	const std::string& expedition_name, const std::string& event_name)
{
	LogExpeditionsDetail("Deleting members lockout: [{}]:[{}]", expedition_name, event_name);

	std::string query_character_ids;
	for (const auto& member : members)
	{
		fmt::format_to(std::back_inserter(query_character_ids), "{},", member.char_id);
	}

	if (!query_character_ids.empty())
	{
		query_character_ids.pop_back(); // trailing comma

		auto query = fmt::format(SQL(
			DELETE FROM expedition_character_lockouts
			WHERE character_id
				IN ({})
				AND is_pending = FALSE
				AND expedition_name = '{}'
				AND event_name = '{}';
		), query_character_ids, EscapeString(expedition_name), EscapeString(event_name));

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::AssignPendingLockouts(uint32_t character_id, const std::string& expedition_name)
{
	LogExpeditionsDetail("Assigning character [{}] pending lockouts [{}]", character_id, expedition_name);

	auto query = fmt::format(SQL(
		UPDATE expedition_character_lockouts
		SET is_pending = FALSE
		WHERE
			character_id = {}
			AND is_pending = TRUE
			AND expedition_name = '{}';
	), character_id, EscapeString(expedition_name));

	database.QueryDatabase(query);
}

void ExpeditionDatabase::DeletePendingLockouts(uint32_t character_id)
{
	LogExpeditionsDetail("Deleting character [{}] pending lockouts", character_id);

	auto query = fmt::format(SQL(
		DELETE FROM expedition_character_lockouts
		WHERE character_id = {} AND is_pending = TRUE;
	), character_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::DeleteAllMembersPendingLockouts(const std::vector<ExpeditionMember>& members)
{
	LogExpeditionsDetail("Deleting pending lockouts for [{}] characters", members.size());

	std::string query_character_ids;
	for (const auto& member : members)
	{
		fmt::format_to(std::back_inserter(query_character_ids), "{},", member.char_id);
	}

	if (!query_character_ids.empty())
	{
		query_character_ids.pop_back(); // trailing comma

		auto query = fmt::format(SQL(
			DELETE FROM expedition_character_lockouts
			WHERE character_id IN ({}) AND is_pending = TRUE;
		), query_character_ids);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::DeleteLockout(uint32_t expedition_id, const std::string& event_name)
{
	LogExpeditionsDetail("Deleting expedition [{}] lockout event [{}]", expedition_id, event_name);

	auto query = fmt::format(SQL(
		DELETE FROM expedition_lockouts
		WHERE expedition_id = {} AND event_name = '{}';
	), expedition_id, EscapeString(event_name));

	database.QueryDatabase(query);
}

uint32_t ExpeditionDatabase::GetExpeditionIDFromCharacterID(uint32_t character_id)
{
	LogExpeditionsDetail("Getting expedition id for character [{}]", character_id);

	uint32_t expedition_id = 0;
	auto query = fmt::format(SQL(
		SELECT expedition_id FROM expedition_members WHERE character_id = {};
	), character_id);

	auto results = database.QueryDatabase(query);
	if (results.Success() && results.RowCount() > 0)
	{
		auto row = results.begin();
		expedition_id = strtoul(row[0], nullptr, 10);
	}
	return expedition_id;
}

ExpeditionMember ExpeditionDatabase::GetExpeditionLeader(uint32_t expedition_id)
{
	LogExpeditionsDetail("Getting expedition leader for expedition [{}]", expedition_id);

	auto query = fmt::format(SQL(
		SELECT expedition_details.leader_id, character_data.name
		FROM expedition_details
			INNER JOIN character_data ON expedition_details.leader_id = character_data.id
		WHERE expedition_id = {}
	), expedition_id);

	ExpeditionMember leader;
	auto results = database.QueryDatabase(query);
	if (results.Success() && results.RowCount() > 0)
	{
		auto row = results.begin();
		leader.char_id = strtoul(row[0], nullptr, 10);
		leader.name    = row[1];
	}
	return leader;
}

void ExpeditionDatabase::InsertCharacterLockouts(
	uint32_t character_id, const std::vector<ExpeditionLockoutTimer>& lockouts,
	bool replace_timer, bool is_pending)
{
	LogExpeditionsDetail("Inserting [{}] lockouts for character [{}]", lockouts.size(), character_id);

	std::string insert_values;
	for (const auto& lockout : lockouts)
	{
		fmt::format_to(std::back_inserter(insert_values),
			"({}, FROM_UNIXTIME({}), {}, '{}', '{}', '{}', {}),",
			character_id,
			lockout.GetExpireTime(),
			lockout.GetDuration(),
			lockout.GetExpeditionUUID(),
			EscapeString(lockout.GetExpeditionName()),
			EscapeString(lockout.GetEventName()),
			is_pending
		);
	}

	if (!insert_values.empty())
	{
		insert_values.pop_back(); // trailing comma

		std::string on_duplicate;
		if (replace_timer)
		{
			on_duplicate = SQL(
				from_expedition_uuid = VALUES(from_expedition_uuid),
				expire_time = VALUES(expire_time),
				duration = VALUES(duration)
			);
		}
		else
		{
			on_duplicate = "character_id = VALUES(character_id)";
		}

		auto query = fmt::format(SQL(
			INSERT INTO expedition_character_lockouts
				(
					character_id,
					expire_time,
					duration,
					from_expedition_uuid,
					expedition_name,
					event_name,
					is_pending
				)
			VALUES {}
			ON DUPLICATE KEY UPDATE {};
		), insert_values, on_duplicate);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::InsertMembersLockout(
	const std::vector<ExpeditionMember>& members, const ExpeditionLockoutTimer& lockout)
{
	LogExpeditionsDetail(
		"Inserting members lockout [{}]:[{}] with expire time [{}]",
		lockout.GetExpeditionName(), lockout.GetEventName(), lockout.GetExpireTime()
	);

	std::string insert_values;
	for (const auto& member : members)
	{
		fmt::format_to(std::back_inserter(insert_values),
			"({}, FROM_UNIXTIME({}), {}, '{}', '{}', '{}'),",
			member.char_id,
			lockout.GetExpireTime(),
			lockout.GetDuration(),
			lockout.GetExpeditionUUID(),
			EscapeString(lockout.GetExpeditionName()),
			EscapeString(lockout.GetEventName())
		);
	}

	if (!insert_values.empty())
	{
		insert_values.pop_back(); // trailing comma

		auto query = fmt::format(SQL(
			INSERT INTO expedition_character_lockouts
				(character_id, expire_time, duration, from_expedition_uuid, expedition_name, event_name)
			VALUES {}
			ON DUPLICATE KEY UPDATE
				from_expedition_uuid = VALUES(from_expedition_uuid),
				expire_time = VALUES(expire_time),
				duration = VALUES(duration);
		), insert_values);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::InsertLockout(
	uint32_t expedition_id, const ExpeditionLockoutTimer& lockout)
{
	LogExpeditionsDetail(
		"Inserting expedition [{}] lockout: [{}]:[{}] expire time: [{}]",
		expedition_id, lockout.GetExpeditionName(), lockout.GetEventName(), lockout.GetExpireTime()
	);

	auto query = fmt::format(SQL(
		INSERT INTO expedition_lockouts
			(expedition_id, from_expedition_uuid, event_name, expire_time, duration)
		VALUES
			({}, '{}', '{}', FROM_UNIXTIME({}), {})
		ON DUPLICATE KEY UPDATE
			from_expedition_uuid = VALUES(from_expedition_uuid),
			expire_time = VALUES(expire_time),
			duration = VALUES(duration);
	),
		expedition_id,
		lockout.GetExpeditionUUID(),
		EscapeString(lockout.GetEventName()),
		lockout.GetExpireTime(),
		lockout.GetDuration()
	);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::InsertLockouts(
	uint32_t expedition_id, const std::unordered_map<std::string, ExpeditionLockoutTimer>& lockouts)
{
	LogExpeditionsDetail("Inserting expedition [{}] lockouts", expedition_id);

	std::string insert_values;
	for (const auto& lockout : lockouts)
	{
		fmt::format_to(std::back_inserter(insert_values),
			"({}, '{}', '{}', FROM_UNIXTIME({}), {}),",
			expedition_id,
			lockout.second.GetExpeditionUUID(),
			EscapeString(lockout.second.GetEventName()),
			lockout.second.GetExpireTime(),
			lockout.second.GetDuration()
		);
	}

	if (!insert_values.empty())
	{
		insert_values.pop_back(); // trailing comma

		auto query = fmt::format(SQL(
			INSERT INTO expedition_lockouts
				(expedition_id, from_expedition_uuid, event_name, expire_time, duration)
			VALUES {}
			ON DUPLICATE KEY UPDATE
				from_expedition_uuid = VALUES(from_expedition_uuid),
				expire_time = VALUES(expire_time),
				duration = VALUES(duration);
		), insert_values);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::InsertMember(uint32_t expedition_id, uint32_t character_id)
{
	LogExpeditionsDetail("Inserting character [{}] into expedition [{}]", character_id, expedition_id);

	auto query = fmt::format(SQL(
		INSERT INTO expedition_members
			(expedition_id, character_id)
		VALUES
			({}, {})
		ON DUPLICATE KEY UPDATE character_id = VALUES(character_id);
	), expedition_id, character_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::InsertMembers(
	uint32_t expedition_id, const std::vector<ExpeditionMember>& members)
{
	LogExpeditionsDetail("Inserting characters into expedition [{}]", expedition_id);

	std::string insert_values;
	for (const auto& member : members)
	{
		fmt::format_to(std::back_inserter(insert_values),
			"({}, {}),",
			expedition_id, member.char_id
		);
	}

	if (!insert_values.empty())
	{
		insert_values.pop_back(); // trailing comma

		auto query = fmt::format(SQL(
			INSERT INTO expedition_members
				(expedition_id, character_id)
			VALUES {};
		), insert_values);

		database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::UpdateLeaderID(uint32_t expedition_id, uint32_t leader_id)
{
	LogExpeditionsDetail("Updating leader [{}] for expedition [{}]", leader_id, expedition_id);

	auto query = fmt::format(SQL(
		UPDATE expedition_details SET leader_id = {} WHERE id = {};
	), leader_id, expedition_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::UpdateLockState(uint32_t expedition_id, bool is_locked)
{
	LogExpeditionsDetail("Updating lock state [{}] for expedition [{}]", is_locked, expedition_id);

	auto query = fmt::format(SQL(
		UPDATE expedition_details SET is_locked = {} WHERE id = {};
	), is_locked, expedition_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::DeleteMember(uint32_t expedition_id, uint32_t character_id)
{
	LogExpeditionsDetail("Removing member [{}] from expedition [{}]", character_id, expedition_id);

	auto query = fmt::format(SQL(
		DELETE FROM expedition_members WHERE expedition_id = {} AND character_id = {};
	), expedition_id, character_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::DeleteAllMembers(uint32_t expedition_id)
{
	LogExpeditionsDetail("Updating all members of expedition [{}] as removed", expedition_id);

	auto query = fmt::format(SQL(
		DELETE FROM expedition_members WHERE expedition_id = {};
	), expedition_id);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::UpdateReplayLockoutOnJoin(uint32_t expedition_id, bool add_on_join)
{
	LogExpeditionsDetail("Updating replay lockout on join [{}] for expedition [{}]", add_on_join, expedition_id);

	auto query = fmt::format(SQL(
		UPDATE expedition_details SET add_replay_on_join = {} WHERE id = {};
	), add_on_join, expedition_id);

	database.QueryDatabase(query);
}
