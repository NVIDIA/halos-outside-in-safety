/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/timestamp.pb.h>

#include "protobuf_util.h"

using namespace google::protobuf;

// helper struct used in accessing the field to set an unique field if just field name is given
typedef struct
{
    Message *message = nullptr;
    const FieldDescriptor *field_desc = nullptr;
    int count = 0;
} FieldMatch;

// helper function to check given index can access a repeated field's value
bool isValidRepeatedIndex(const Message &message, const FieldDescriptor *field_desc, const Reflection *reflection, int index, Result &result)
{
    int n = reflection->FieldSize(message, field_desc);
    if (n == 0)
    {
        result.type = VALUE_TYPE_ERROR;
        std::cerr << "ERROR : The repeated field '" + field_desc->name() + "' contains no elements." << std::endl;
        return false;
    }
    else if (index < 0 || index >= n)
    {
        result.type = VALUE_TYPE_ERROR;
        std::cerr << "ERROR : Index " + std::to_string(index) + " is out of bounds for repeated field '" + field_desc->name() + "' (size:  " + std::to_string(n) + "  )." << std::endl;
        return false;
    }
    else
        return true;
}

// helper function to check if the field has been set
bool checkFieldPresence(const Message &message, const FieldDescriptor *field_desc, const Reflection *reflection, Result &result)
{
    // In proto3, scalar fields (int, float, bool, string, enum) do not support HasField()
    // and always have a default value. Only check presence for message-type fields.
    if (field_desc->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
    {
        if (!reflection->HasField(message, field_desc))
        {
            result.type = VALUE_TYPE_ERROR;
            std::cerr << "ERROR : The field '" + field_desc->name() + "' is not set in the message." << std::endl;
            return false;
        }
    }
    return true;
}

// helper function to get a field value based on field type
void checkFieldType(const Message *message, const FieldDescriptor *field_desc, const Reflection *reflection, Result &result, int index, char *keyStr)
{
    switch (field_desc->type())
    {
    case FieldDescriptor::TYPE_STRING:
        result.type = VALUE_TYPE_STRING;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.s = strdup(reflection->GetRepeatedString(*message, field_desc, index).c_str());
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.s = strdup(reflection->GetString(*message, field_desc).c_str());
        }
        break;
    case FieldDescriptor::TYPE_FLOAT:
        result.type = VALUE_TYPE_FLOAT;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.f = reflection->GetRepeatedFloat(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.f = reflection->GetFloat(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_DOUBLE:
        result.type = VALUE_TYPE_DOUBLE;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.d = reflection->GetRepeatedDouble(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.d = reflection->GetDouble(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_INT32:
        result.type = VALUE_TYPE_INT32;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.i32 = reflection->GetRepeatedInt32(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.i32 = reflection->GetInt32(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_INT64:
        result.type = VALUE_TYPE_INT64;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.i64 = reflection->GetRepeatedInt64(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.i64 = reflection->GetInt64(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_UINT32:
        result.type = VALUE_TYPE_UINT32;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.u32 = reflection->GetRepeatedUInt32(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.u32 = reflection->GetUInt32(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_UINT64:
        result.type = VALUE_TYPE_UINT64;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.u64 = reflection->GetRepeatedUInt64(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.u64 = reflection->GetUInt64(*message, field_desc);
        }
        break;
    case FieldDescriptor::TYPE_BOOL:
        result.type = VALUE_TYPE_BOOL;
        if (field_desc->is_repeated() && isValidRepeatedIndex(*message, field_desc, reflection, index, result))
        {
            result.data.b = reflection->GetRepeatedBool(*message, field_desc, index);
        }
        else if (!field_desc->is_repeated() && checkFieldPresence(*message, field_desc, reflection, result))
        {
            result.data.b = reflection->GetBool(*message, field_desc);
        }
        break;
    default:
        if (field_desc->message_type()->full_name() == "google.protobuf.Timestamp")
        {
            const Message &ts_msg = reflection->GetMessage(*message, field_desc);
            const Timestamp &ts = static_cast<const Timestamp &>(ts_msg);
            result.type = VALUE_TYPE_TIMESTAMP;
            result.data.timestamp.seconds = ts.seconds();
            result.data.timestamp.nanos = ts.nanos();
        }
        else if (field_desc->is_map())
        {
            if (strcmp(keyStr, " ") == 0)
            {
                std::cerr << "ERROR : A key must be specified to retrieve a value from the map field '" + field_desc->name() + "'." << std::endl;
                result.type = VALUE_TYPE_ERROR;
                break;
            }

            int size = reflection->FieldSize(*message, field_desc);
            const Descriptor *entry_desc = field_desc->message_type();
            const FieldDescriptor *key_field = entry_desc->map_key();
            const FieldDescriptor *value_field = entry_desc->map_value();

            for (int i = 0; i < size; ++i)
            {
                const Message &entry = reflection->GetRepeatedMessage(*message, field_desc, i);
                std::string message_key = entry.GetReflection()->GetString(entry, key_field);

                if (message_key == keyStr)
                {
                    result.type = VALUE_TYPE_MAPVALUE;
                    result.data.mapValue = strdup(entry.GetReflection()->GetString(entry, value_field).c_str());
                    break;
                }
            }
            if (result.type != VALUE_TYPE_MAPVALUE)
            {
                // Key not found in map is an expected condition (not all messages have every key).
                // Return VALUE_TYPE_ERROR silently so callers can handle it gracefully.
                result.type = VALUE_TYPE_ERROR;
            }
        }
        else
        {
            std::cerr << "ERROR : No matching field type found for the specified field '" + field_desc->name() + "'" << std::endl;
            result.type = VALUE_TYPE_ERROR;
        }
    }
}

// helper function to search for a field in the message recursively given only the field name
void findField(const Message &message, char *field_name, int index, char *keyStr, Result &result, int *cnt)
{
    const Descriptor *descriptor = message.GetDescriptor();
    const Reflection *reflection = message.GetReflection();
    const FieldDescriptor *field_desc = descriptor->FindFieldByName(field_name);

    if (field_desc)
    {
        checkFieldType(&message, field_desc, reflection, result, index, keyStr);
        *cnt = *cnt + 1;
        // Field found at current level — do not recurse into child messages.
        // This matches direct proto accessor behavior (e.g. obj.confidence() returns
        // the Object's confidence, not a nested Bbox's confidence).
        return;
    }

    for (int i = 0; i < descriptor->field_count(); ++i)
    {
        const FieldDescriptor *child = descriptor->field(i);
        if (child->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
        {
            if (child->is_repeated())
            {
                int n = reflection->FieldSize(message, child);
                for (int j = 0; j < n; ++j)
                {
                    const Message &nested = reflection->GetRepeatedMessage(message, child, j);
                    findField(nested, field_name, index, keyStr, result, cnt);
                }
            }
            else if (reflection->HasField(message, child))
            {
                const Message &nested = reflection->GetMessage(message, child);
                findField(nested, field_name, index, keyStr, result, cnt);
            }
        }
    }
}

// API function to retrieve a field's value given its field name or path from a message
Result getFieldValue(const Message &message, char *path)
{
    Result result;
    std::string pathString(path);
    size_t isFullPath = pathString.find('.');

    if (isFullPath == std::string::npos)
    {
        int cnt = 0;
        int index = 0;
        char *keyStr = strdup(" ");
        bool keyInit = false;
        bool pathInit = false;

        std::string pathString(path);
        size_t bracketStart = pathString.find('[');
        if (bracketStart != std::string::npos)
        {
            if (path[strlen(path) - 1] != ']')
            {
                std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                result.type = VALUE_TYPE_ERROR;
                return result;
            }
            else
            {
                if (path[bracketStart + 1] == '\'')
                {
                    if (path[strlen(path) - 2] != '\'')
                    {
                        std::cerr << "ERROR : The map key must be enclosed in single quotes." << std::endl;
                        result.type = VALUE_TYPE_ERROR;
                        return result;
                    }
                    else
                    {
                        keyStr = strdup(pathString.substr(bracketStart + 2, strlen(path) - bracketStart - 4).c_str());
                        keyInit = true;
                        path = strdup(pathString.substr(0, bracketStart).c_str());
                        pathInit = true;
                    }
                }
                else
                {
                    std::string indexString = pathString.substr(bracketStart + 1, strlen(path) - bracketStart - 1);
                    index = stoi(indexString);
                    path = strdup(pathString.substr(0, bracketStart).c_str());
                    pathInit = true;
                }
            }
        }

        findField(message, path, index, keyStr, result, &cnt);

        if (cnt == 0)
        {
            std::cerr << "ERROR : Field '" + std::string(path) + "' is not present in the message." << std::endl;
            result.type = VALUE_TYPE_ERROR;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return result;
        }
        else if (cnt == 1)
        {
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return result;
        }
        else
        {
            std::cerr << "ERROR : Multiple fields named '" + std::string(path) + "' are present in the message. Specify the full field path." << std::endl;
            result.type = VALUE_TYPE_ERROR;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return result;
        }
    }
    else
    {
        const Message *current = &message;
        char *saveptr = nullptr;
        char *token = strtok_r(path, ".", &saveptr);
        bool tokenInit = false;
        bool keyInit = false;
        char *keyStr = strdup(" ");

        const Descriptor *desc = message.GetDescriptor();
        std::string msgName = desc->name();
        std::transform(msgName.begin(), msgName.end(), msgName.begin(), ::tolower);
        if (msgName == token)
            token = strtok_r(NULL, ".", &saveptr);

        while (token != nullptr)
        {
            int index = 0;
            tokenInit = false;
            keyInit = false;

            std::string tokenString(token);
            size_t bracketStart = tokenString.find('[');
            if (bracketStart != std::string::npos)
            {
                if (token[strlen(token) - 1] != ']')
                {
                    std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                    result.type = VALUE_TYPE_ERROR;
                    return result;
                }
                else
                {
                    if (token[bracketStart + 1] == '\'')
                    {
                        if (token[strlen(token) - 2] != '\'')
                        {
                            std::cerr << "ERROR : The map key must be enclosed in single quotes.";
                            result.type = VALUE_TYPE_ERROR;
                            return result;
                        }
                        else
                        {
                            keyStr = strdup(tokenString.substr(bracketStart + 2, strlen(token) - bracketStart - 4).c_str());
                            token = strdup(tokenString.substr(0, bracketStart).c_str());
                            keyInit = true;
                            tokenInit = true;
                        }
                    }
                    else
                    {
                        std::string indexString = tokenString.substr(bracketStart + 1, strlen(token) - bracketStart - 1);
                        index = stoi(indexString);
                        token = strdup(tokenString.substr(0, bracketStart).c_str());
                        tokenInit = true;
                    }
                }
            }

            const Descriptor *descriptor = current->GetDescriptor();
            const Reflection *reflection = current->GetReflection();
            const FieldDescriptor *field_desc = descriptor->FindFieldByName(token);

            if (!field_desc)
            {
                std::cerr << "ERROR : Field '" + std::string(token) + "' not found in message." << std::endl;
                result.type = VALUE_TYPE_ERROR;
                if (tokenInit)
                    free((void *)token);
                return result;
            }

            char *next_token = strtok_r(nullptr, ".", &saveptr);
            if (next_token)
            {
                if (field_desc->type() != FieldDescriptor::TYPE_MESSAGE)
                {
                    std::cerr << "ERROR : Field '" + std::string(token) + "' is not a message type." << std::endl;
                    result.type = VALUE_TYPE_ERROR;
                }
                if (field_desc->is_repeated() && isValidRepeatedIndex(*current, field_desc, reflection, index, result))
                {
                    current = &reflection->GetRepeatedMessage(*current, field_desc, index);
                    token = next_token;
                    continue;
                }
                else if (!field_desc->is_repeated() &&
                         field_desc->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
                         !reflection->HasField(*current, field_desc))
                {
                    std::cerr << "ERROR : Field '" + std::string(token) + "' is not set in the message." << std::endl;
                    result.type = VALUE_TYPE_ERROR;
                }
                else if (!field_desc->is_repeated())
                {
                    current = &reflection->GetMessage(*current, field_desc);
                    token = next_token;
                    continue;
                }
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return result;
            }
            else
            {
                checkFieldType(current, field_desc, reflection, result, index, keyStr);
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return result;
            }
        }
        std::cerr << "ERROR : Failed to parse the input." << std::endl;
        result.type = VALUE_TYPE_ERROR;
        if (tokenInit)
            free((void *)token);
        if (keyInit)
            free((void *)keyStr);
        return result;
    }
}

// helper function to check if a field is set based on its field type
bool isFieldPresent(const Message *message, const FieldDescriptor *field_desc, const Reflection *reflection, int index, char *keyStr)
{
    if (field_desc->is_map())
    {
        if (strcmp(keyStr, " ") == 0)
        {
            std::cerr << "ERROR : A key must be specified to retrieve a value from the map field '" + field_desc->name() + "'." << std::endl;
            return false;
        }

        int size = reflection->FieldSize(*message, field_desc);
        const Descriptor *entry_desc = field_desc->message_type();
        const FieldDescriptor *key_field = entry_desc->map_key();

        for (int i = 0; i < size; ++i)
        {
            const Message &entry = reflection->GetRepeatedMessage(*message, field_desc, i);
            std::string message_key = entry.GetReflection()->GetString(entry, key_field);

            if (message_key == keyStr)
            {
                return true;
            }
        }
        return false;
    }
    else if (field_desc->is_repeated())
    {
        int n = reflection->FieldSize(*message, field_desc);
        if (index < n)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        if (reflection->HasField(*message, field_desc))
        {
            return true;
        }
        else
        {
            return false;
        }
    }
}

// helper function to search for a field in the message recursively given only the field name
void findFieldForFieldPresence(const Message &message, char *field_name, int index, char *keyStr, int *cnt, bool *isPresentUnique)
{
    const Descriptor *descriptor = message.GetDescriptor();
    const Reflection *reflection = message.GetReflection();
    const FieldDescriptor *field_desc = descriptor->FindFieldByName(field_name);

    if (field_desc)
    {
        *cnt = *cnt + 1;
        if (*cnt == 1)
            *isPresentUnique = isFieldPresent(&message, field_desc, reflection, index, keyStr);
        return;
    }

    for (int i = 0; i < descriptor->field_count(); ++i)
    {
        const FieldDescriptor *child = descriptor->field(i);
        if (child->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
        {
            if (child->is_repeated())
            {
                int n = reflection->FieldSize(message, child);
                for (int j = 0; j < n; ++j)
                {
                    const Message &nested = reflection->GetRepeatedMessage(message, child, j);
                    findFieldForFieldPresence(nested, field_name, index, keyStr, cnt, isPresentUnique);
                }
            }
            else if (reflection->HasField(message, child))
            {
                const Message &nested = reflection->GetMessage(message, child);
                findFieldForFieldPresence(nested, field_name, index, keyStr, cnt, isPresentUnique);
            }
        }
    }
}

// helper function to check given index can access a repeated field's value
bool isValidRepeatedIndexForFieldPresence(const Message &message, const FieldDescriptor *field_desc, const Reflection *reflection, int index)
{
    int n = reflection->FieldSize(message, field_desc);
    if (n == 0)
    {
        std::cerr << "ERROR : The repeated field '" + field_desc->name() + "' contains no elements." << std::endl;
        return false;
    }
    else if (index < 0 || index >= n)
    {
        std::cerr << "ERROR : Index " + std::to_string(index) + " is out of bounds for repeated field '" + field_desc->name() + "' (size:  " + std::to_string(n) + "  )." << std::endl;
        return false;
    }
    else
        return true;
}

// API function to check if the field is set given the field name or field path from a message
bool getFieldPresence(const Message &message, char *path)
{
    std::string pathString(path);
    size_t isFullPath = pathString.find('.');
    int index = 0;
    char *keyStr = strdup(" ");
    bool isPresentUnique = false;

    if (isFullPath == std::string::npos)
    {
        int cnt = 0;
        std::string pathString(path);
        size_t bracketStart = pathString.find('[');
        bool keyInit = false;
        bool pathInit = false;

        if (bracketStart != std::string::npos)
        {
            if (path[strlen(path) - 1] != ']')
            {
                std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                return false;
            }
            else
            {
                if (path[bracketStart + 1] == '\'')
                {
                    if (path[strlen(path) - 2] != '\'')
                    {
                        std::cerr << "ERROR : The map key must be enclosed in single quotes." << std::endl;
                        return false;
                    }
                    else
                    {
                        keyStr = strdup(pathString.substr(bracketStart + 2, strlen(path) - bracketStart - 4).c_str());
                        keyInit = true;
                        path = strdup(pathString.substr(0, bracketStart).c_str());
                        pathInit = true;
                    }
                }
                else
                {
                    std::string indexString = pathString.substr(bracketStart + 1, strlen(path) - bracketStart - 1);
                    index = stoi(indexString);
                    path = strdup(pathString.substr(0, bracketStart).c_str());
                    pathInit = true;
                }
            }
        }

        findFieldForFieldPresence(message, path, index, keyStr, &cnt, &isPresentUnique);

        if (cnt == 0)
        {
            std::cerr << "ERROR : Field is not present in the message." << std::endl;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return false;
        }
        else if (cnt == 1)
        {
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return isPresentUnique;
        }
        else
        {
            std::cerr << "ERROR : Multiple fields named '" + std::string(path) + "' are present in the message. Specify the full field path." << std::endl;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
            return false;
        }
    }
    else
    {
        const Message *current = &message;
        char *saveptr = nullptr;
        char *token = strtok_r(path, ".", &saveptr);
        bool tokenInit = false;
        bool keyInit = false;

        const Descriptor *desc = message.GetDescriptor();
        std::string msgName = desc->name();
        std::transform(msgName.begin(), msgName.end(), msgName.begin(), ::tolower);
        if (msgName == token)
            token = strtok_r(NULL, ".", &saveptr);

        while (token != nullptr)
        {
            std::string tokenString(token);
            size_t bracketStart = tokenString.find('[');
            tokenInit = false;
            keyInit = false;

            if (bracketStart != std::string::npos)
            {
                if (token[strlen(token) - 1] != ']')
                {
                    std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                    return false;
                }
                else
                {
                    if (token[bracketStart + 1] == '\'')
                    {
                        if (token[strlen(token) - 2] != '\'')
                        {
                            std::cerr << "ERROR : The map key must be enclosed in single quotes." << std::endl;
                            return false;
                        }
                        else
                        {
                            keyStr = strdup(tokenString.substr(bracketStart + 2, strlen(token) - bracketStart - 4).c_str());
                            keyInit = true;
                            token = strdup(tokenString.substr(0, bracketStart).c_str());
                            tokenInit = true;
                        }
                    }
                    else
                    {
                        std::string indexString = tokenString.substr(bracketStart + 1, strlen(token) - bracketStart - 1);
                        index = stoi(indexString);
                        token = strdup(tokenString.substr(0, bracketStart).c_str());
                        tokenInit = true;
                    }
                }
            }

            const Descriptor *descriptor = current->GetDescriptor();
            const Reflection *reflection = current->GetReflection();
            const FieldDescriptor *field_desc = descriptor->FindFieldByName(token);

            if (!field_desc)
            {
                std::cerr << "ERROR : Field '" + std::string(token) + "' not found in message." << std::endl;
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return false;
            }

            char *next_token = strtok_r(nullptr, ".", &saveptr);
            if (next_token)
            {
                if (field_desc->type() != FieldDescriptor::TYPE_MESSAGE)
                {
                    std::cerr << "ERROR : Field '" + std::string(token) + "' is not a message type." << std::endl;
                }
                else if (field_desc->is_repeated() && isValidRepeatedIndexForFieldPresence(*current, field_desc, reflection, index))
                {
                    current = &reflection->GetRepeatedMessage(*current, field_desc, index);
                    token = next_token;
                    continue;
                }
                else if (!field_desc->is_repeated() &&
                         field_desc->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
                         !reflection->HasField(*current, field_desc))
                {
                    std::cerr << "ERROR : Field '" + std::string(token) + "' is not set in the message." << std::endl;
                }
                else if (!field_desc->is_repeated())
                {
                    current = &reflection->GetMessage(*current, field_desc);
                    token = next_token;
                    continue;
                }
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return false;
            }
            else
            {
                bool result = isFieldPresent(current, field_desc, reflection, index, keyStr);
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return result;
            }
        }
        std::cerr << "ERROR : Failed to parse the input." << std::endl;
        if (tokenInit)
            free((void *)token);
        if (keyInit)
            free((void *)keyStr);
        return false;
    }
}

// helper function to check if repeated field can be set given the index
bool isValidRepeatedIndexForSet(const Message &message, const FieldDescriptor *field_desc, const Reflection *reflection, int index)
{
    int n = reflection->FieldSize(message, field_desc);
    if (n == 0)
        return true;
    if (index < 0 || index >= n)
    {
        std::cerr << "ERROR : Index " + std::to_string(index) + " is out of bounds for repeated field '" + field_desc->name() + "' (size:  " + std::to_string(n) + "  )." << std::endl;

        return false;
    }
    else
        return true;
}

// helper function to set the field based on the field type
void checkFieldTypeToSet(Message *message, const FieldDescriptor *field_desc, const Reflection *reflection, FieldData fieldData, int index, char *keyStr, bool indexNotSet)
{
    switch (field_desc->type())
    {
    case FieldDescriptor::TYPE_STRING:
        if (fieldData.type == VALUE_TYPE_STRING)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedString(message, field_desc, index, fieldData.data.s);
                }
                else
                {
                    reflection->AddString(message, field_desc, fieldData.data.s);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetString(message, field_desc, fieldData.data.s);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type string." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_FLOAT:
        if (fieldData.type == VALUE_TYPE_FLOAT)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedFloat(message, field_desc, index, fieldData.data.f);
                }
                else
                {
                    reflection->AddFloat(message, field_desc, fieldData.data.f);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetFloat(message, field_desc, fieldData.data.f);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type float." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_DOUBLE:
        if (fieldData.type == VALUE_TYPE_DOUBLE)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedDouble(message, field_desc, index, fieldData.data.d);
                }
                else
                {
                    reflection->AddDouble(message, field_desc, fieldData.data.d);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetDouble(message, field_desc, fieldData.data.d);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type double." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_INT32:
        if (fieldData.type == VALUE_TYPE_INT32)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedInt32(message, field_desc, index, fieldData.data.i32);
                }
                else
                {
                    reflection->AddInt32(message, field_desc, fieldData.data.i32);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetInt32(message, field_desc, fieldData.data.i32);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type int32." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_INT64:
        if (fieldData.type == VALUE_TYPE_INT64)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedInt64(message, field_desc, index, fieldData.data.i64);
                }
                else
                {
                    reflection->AddInt64(message, field_desc, fieldData.data.i64);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetInt64(message, field_desc, fieldData.data.i64);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type int64." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_UINT32:
        if (fieldData.type == VALUE_TYPE_UINT32)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedUInt32(message, field_desc, index, fieldData.data.u32);
                }
                else
                {
                    reflection->AddUInt32(message, field_desc, fieldData.data.u32);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetUInt32(message, field_desc, fieldData.data.u32);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type uint32." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_UINT64:
        if (fieldData.type == VALUE_TYPE_UINT64)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedUInt64(message, field_desc, index, fieldData.data.u64);
                }
                else
                {
                    reflection->AddUInt64(message, field_desc, fieldData.data.u64);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetUInt64(message, field_desc, fieldData.data.u64);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type uint64." << std::endl;
        }
        break;
    case FieldDescriptor::TYPE_BOOL:
        if (fieldData.type == VALUE_TYPE_BOOL)
        {
            if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*message, field_desc, reflection, index))
            {
                if (!indexNotSet)
                {
                    reflection->SetRepeatedBool(message, field_desc, index, fieldData.data.b);
                }
                else
                {
                    reflection->AddBool(message, field_desc, fieldData.data.b);
                }
            }
            else if (!field_desc->is_repeated())
            {
                reflection->SetBool(message, field_desc, fieldData.data.b);
            }
        }
        else
        {
            std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type boolean." << std::endl;
        }
        break;
    default:
        if (field_desc->message_type()->full_name() == "google.protobuf.Timestamp")
        {
            if (fieldData.type == VALUE_TYPE_TIMESTAMP)
            {
                Message *ts_msg = reflection->MutableMessage(message, field_desc);
                Timestamp *ts = static_cast<Timestamp *>(ts_msg);
                if (ts)
                {
                    ts->set_seconds(fieldData.data.timestamp->seconds);
                    ts->set_nanos(fieldData.data.timestamp->nanos);
                }
            }
            else
            {
                std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type TimeStamp (seconds and nanos)." << std::endl;
                break;
            }
        }
        else if (field_desc->is_map())
        {
            if (fieldData.type == VALUE_TYPE_MAPVALUE)
            {
                if (strcmp(keyStr, " ") == 0)
                {
                    std::cerr << "ERROR : A key must be specified to retrieve a value from the map field '" + field_desc->name() + "'." << std::endl;
                    break;
                }

                const Descriptor *entry_desc = field_desc->message_type();
                const FieldDescriptor *key_field = entry_desc->map_key();
                const FieldDescriptor *value_field = entry_desc->map_value();
                bool updated = false;
                int size = reflection->FieldSize(*message, field_desc);
                for (int i = 0; i < size; ++i)
                {
                    Message *entry = reflection->MutableRepeatedMessage(message, field_desc, i);
                    std::string existing_key = entry->GetReflection()->GetString(*entry, key_field);
                    if (existing_key == keyStr)
                    {
                        entry->GetReflection()->SetString(entry, value_field, fieldData.data.mapValue);
                        updated = true;
                        break;
                    }
                }
                if (!updated)
                {
                    Message *entry = reflection->AddMessage(message, field_desc);
                    entry->GetReflection()->SetString(entry, key_field, keyStr);
                    entry->GetReflection()->SetString(entry, value_field, fieldData.data.mapValue);
                }
            }
            else
            {
                std::cerr << "ERROR : The field '" + field_desc->name() + "' requires a value of type string." << std::endl;
            }
        }
        else
        {
            std::cerr << "ERROR : No matching field descriptor found for the specified field '" + field_desc->name() + "'." << std::endl;
        }
    }
}

// helper function to search for a field in the message recursively given only the field name
void findFieldForSet(Message *message, const std::string &field_name, FieldMatch &match)
{
    const auto *descriptor = message->GetDescriptor();
    const auto *reflection = message->GetReflection();
    const auto *field_desc = descriptor->FindFieldByName(field_name);

    if (field_desc)
    {
        match.count++;
        if (match.count == 1)
        {
            match.message = message;
            match.field_desc = field_desc;
        }
        return;
    }

    for (int i = 0; i < descriptor->field_count(); ++i)
    {
        const auto *child_field = descriptor->field(i);
        if (child_field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE)
        {
            if (child_field->is_repeated())
            {
                int n = reflection->FieldSize(*message, child_field);
                for (int j = 0; j < n; ++j)
                {
                    findFieldForSet(reflection->MutableRepeatedMessage(message, child_field, j), field_name, match);
                }
            }
            else
            {
                findFieldForSet(reflection->MutableMessage(message, child_field), field_name, match);
            }
        }
    }
}

// API function to set a field value given the fieldname or field path in a message
void setFieldValue(Message &message, char *path, FieldData fieldData)
{
    std::string pathString(path);
    size_t isFullPath = pathString.find('.');

    if (isFullPath == std::string::npos)
    {
        int index = 0;
        bool indexNotSet = true;
        char *keyStr = strdup(" ");
        bool keyInit = false;
        bool pathInit = false;

        std::string pathString(path);
        size_t bracketStart = pathString.find('[');
        if (bracketStart != std::string::npos)
        {
            if (path[strlen(path) - 1] != ']')
            {
                std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                return;
            }
            else
            {
                if (path[bracketStart + 1] == '\'')
                {
                    if (path[strlen(path) - 2] != '\'')
                    {
                        std::cerr << "ERROR : The map key must be enclosed in single quotes." << std::endl;
                        return;
                    }
                    else
                    {
                        keyStr = strdup(pathString.substr(bracketStart + 2, strlen(path) - bracketStart - 4).c_str());
                        path = strdup(pathString.substr(0, bracketStart).c_str());
                        keyInit = true;
                        pathInit = true;
                    }
                }
                else
                {
                    std::string indexString = pathString.substr(bracketStart + 1, strlen(path) - bracketStart - 1);
                    index = stoi(indexString);
                    indexNotSet = false;
                    path = strdup(pathString.substr(0, bracketStart).c_str());
                    pathInit = true;
                }
            }
        }

        FieldMatch match;
        findFieldForSet(&message, path, match);

        if (match.count == 0)
        {
            std::cerr << "ERROR : Field '" + std::string(path) + "' is not present in the message." << std::endl;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
        }
        else if (match.count == 1 && match.message && match.field_desc)
        {
            checkFieldTypeToSet(match.message, match.field_desc, match.message->GetReflection(), fieldData, index, keyStr, indexNotSet);
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
        }
        else
        {
            std::cerr << "ERROR : Multiple fields named '" + std::string(path) + "' are present in the message. Specify the full field path." << std::endl;
            if (pathInit)
                free((void *)path);
            if (keyInit)
                free((void *)keyStr);
        }
    }
    else
    {
        Message *current = &message;
        char *saveptr = nullptr;
        char *token = strtok_r(path, ".", &saveptr);
        char *keyStr = strdup(" ");
        bool keyInit = false;
        bool tokenInit = false;

        const Descriptor *desc = message.GetDescriptor();
        std::string msgName = desc->name();
        std::transform(msgName.begin(), msgName.end(), msgName.begin(), ::tolower);
        if (msgName == token)
            token = strtok_r(NULL, ".", &saveptr);

        while (token != nullptr)
        {
            int index = 0;
            bool indexNotSet = true;
            std::string tokenString(token);
            size_t bracketStart = tokenString.find('[');
            keyInit = false;
            tokenInit = false;

            if (bracketStart != std::string::npos)
            {
                if (token[strlen(token) - 1] != ']')
                {
                    std::cerr << "ERROR : The index value for repeated field access must end with a closing bracket ']'." << std::endl;
                    return;
                }
                else
                {
                    if (token[bracketStart + 1] == '\'')
                    {
                        if (token[strlen(token) - 2] != '\'')
                        {
                            std::cerr << "ERROR : The map key must be enclosed in single quotes." << std::endl;
                            return;
                        }
                        else
                        {
                            keyStr = strdup(tokenString.substr(bracketStart + 2, strlen(token) - bracketStart - 4).c_str());
                            token = strdup(tokenString.substr(0, bracketStart).c_str());
                            keyInit = true;
                            tokenInit = true;
                        }
                    }
                    else
                    {
                        std::string indexString = tokenString.substr(bracketStart + 1, strlen(token) - bracketStart - 1);
                        index = stoi(indexString);
                        token = strdup(tokenString.substr(0, bracketStart).c_str());
                        indexNotSet = false;
                        tokenInit = true;
                    }
                }
            }

            const Descriptor *descriptor = current->GetDescriptor();
            const Reflection *reflection = current->GetReflection();
            const FieldDescriptor *field_desc = descriptor->FindFieldByName(token);

            if (!field_desc)
            {
                std::cerr << "ERROR : Field '" + std::string(token) + "' not found in message." << std::endl;
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return;
            }

            char *next_token = strtok_r(nullptr, ".", &saveptr);
            if (next_token)
            {
                if (field_desc->type() != FieldDescriptor::TYPE_MESSAGE)
                {
                    std::cerr << "ERROR : Field '" + std::string(token) + "' is not a message type." << std::endl;
                }
                if (field_desc->is_repeated() && isValidRepeatedIndexForSet(*current, field_desc, reflection, index))
                {
                    int n = reflection->FieldSize(*current, field_desc);
                    if (n == 0 || indexNotSet)
                        current = reflection->AddMessage(current, field_desc);
                    else
                        current = reflection->MutableRepeatedMessage(current, field_desc, index);
                    token = next_token;
                    continue;
                }
                else if (!field_desc->is_repeated())
                {
                    current = reflection->MutableMessage(current, field_desc);
                    token = next_token;
                    continue;
                }
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return;
            }
            else
            {
                checkFieldTypeToSet(current, field_desc, reflection, fieldData, index, keyStr, indexNotSet);
                if (tokenInit)
                    free((void *)token);
                if (keyInit)
                    free((void *)keyStr);
                return;
            }
        }
        std::cerr << "ERROR : Failed to parse the input.";
        if (tokenInit)
            free((void *)token);
        if (keyInit)
            free((void *)keyStr);
    }
}

// API function to check the versions
bool checkProtobufVersion()
{
    int header_version = GOOGLE_PROTOBUF_VERSION;
    int min_library_version = GOOGLE_PROTOBUF_MIN_LIBRARY_VERSION;

    if (header_version < min_library_version)
    {
        return false;
    }
    return true;
}

// helper function to check the extention of the given file
bool hasExtension(const std::string &filename, const std::string &extension)
{
    if (filename.length() >= extension.length())
    {
        return (0 == filename.compare(filename.length() - extension.length(), extension.length(), extension));
    }
    else
    {
        return false;
    }
}

// API function to get the field value from a proto binary file or a textproto file
Result getFieldValueFromFile(Message &message, char *fileName, char *path)
{
    if (hasExtension(fileName, ".textproto"))
    {
        std::ifstream input(fileName);
        if (!input)
        {
            Result res;
            res.type = VALUE_TYPE_ERROR;
            std::cerr << "ERROR : Can't open the file " << fileName << std::endl;
            return res;
        }
        std::stringstream buffer;
        buffer << input.rdbuf();
        std::string text = buffer.str();

        if (TextFormat::ParseFromString(text, &message))
        {
            return getFieldValue(message, path);
        }
        else
        {
            Result res;
            res.type = VALUE_TYPE_ERROR;
            std::cerr << "ERROR: Failed to parse '" << fileName << "'." << std::endl;
            return res;
        }
    }
    else if (hasExtension(fileName, ".bin"))
    {
        std::ifstream binInput(fileName, std::ios::in | std::ios::binary);
        if (!binInput)
        {
            Result res;
            res.type = VALUE_TYPE_ERROR;
            std::cerr << "ERROR: Could not open file in binary mode." << std::endl;
            return res;
        }

        if (message.ParseFromIstream(&binInput))
        {
            return getFieldValue(message, path);
        }
        else
        {
            Result res;
            res.type = VALUE_TYPE_ERROR;
            std::cerr << "ERROR: Failed to parse '" << fileName << "'." << std::endl;
            return res;
        }
    }
    else
    {
        Result res;
        res.type = VALUE_TYPE_ERROR;
        std::cerr << "ERROR: Unknown file type. Provide either a .textproto file or binary file." << std::endl;
        return res;
    }
}