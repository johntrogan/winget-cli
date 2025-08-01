// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include <pch.h>
#include "YamlWrapper.h"
#include "AppInstallerErrors.h"
#include "AppInstallerLogging.h"
#include "AppInstallerStrings.h"


namespace AppInstaller::YAML::Wrapper
{
    namespace
    {
        Node::Type ConvertNodeType(yaml_node_type_t type)
        {
            switch (type)
            {
            case YAML_NO_NODE:
                return Node::Type::None;
            case YAML_SCALAR_NODE:
                return Node::Type::Scalar;
            case YAML_SEQUENCE_NODE:
                return Node::Type::Sequence;
            case YAML_MAPPING_NODE:
                return Node::Type::Mapping;
            }

            THROW_HR(E_UNEXPECTED);
        }

        Exception::Type ConvertErrorType(yaml_error_type_t type)
        {
            switch (type)
            {
            case YAML_NO_ERROR:
                return Exception::Type::None;
            case YAML_MEMORY_ERROR:
                return Exception::Type::Memory;
            case YAML_READER_ERROR:
                return Exception::Type::Reader;
            case YAML_SCANNER_ERROR:
                return Exception::Type::Scanner;
            case YAML_PARSER_ERROR:
                return Exception::Type::Parser;
            case YAML_COMPOSER_ERROR:
                return Exception::Type::Composer;
            case YAML_WRITER_ERROR:
                return Exception::Type::Writer;
            case YAML_EMITTER_ERROR:
                return Exception::Type::Emitter;
            }

            THROW_HR(E_UNEXPECTED);
        }

        Mark ConvertMark(const yaml_mark_t& mark)
        {
            return { mark.line + 1, mark.column + 1 };
        }

        std::string ConvertYamlString(yaml_char_t* string, const yaml_mark_t& mark, size_t length = std::string::npos)
        {
            std::string_view resultView;

            if (length == std::string::npos)
            {
                resultView = { reinterpret_cast<char*>(string) };
            }
            else
            {
                resultView = { reinterpret_cast<char*>(string), length };
            }

            size_t invalidCharacter = Utility::FindControlCodeToConvert(resultView);
            if (invalidCharacter != std::string::npos)
            {
                THROW_EXCEPTION(Exception(Exception::Type::Policy, "unsupported control character", ConvertMark(mark)));
            }

            return std::string{ resultView };
        }

        std::string ConvertScalarToString(yaml_node_t* node, const yaml_mark_t& mark)
        {
            return ConvertYamlString(node->data.scalar.value, mark, node->data.scalar.length);
        }

        yaml_scalar_style_t ConvertStyle(ScalarStyle style)
        {
            switch (style)
            {
            case ScalarStyle::Any: return yaml_scalar_style_t::YAML_ANY_SCALAR_STYLE;
            case ScalarStyle::Plain: return yaml_scalar_style_t::YAML_PLAIN_SCALAR_STYLE;
            case ScalarStyle::SingleQuoted: return yaml_scalar_style_t::YAML_SINGLE_QUOTED_SCALAR_STYLE;
            case ScalarStyle::DoubleQuoted: return yaml_scalar_style_t::YAML_DOUBLE_QUOTED_SCALAR_STYLE;
            case ScalarStyle::Literal: return yaml_scalar_style_t::YAML_LITERAL_SCALAR_STYLE;
            case ScalarStyle::Folded: return yaml_scalar_style_t::YAML_FOLDED_SCALAR_STYLE;
            default: THROW_HR(E_UNEXPECTED);
            }
        }
    }

    Document::Document(bool init) :
        m_token(true)
    {
        if (init)
        {
            // Initialize with no version directive or tags, and implicit start and end.
            if (!yaml_document_initialize(&m_document, NULL, NULL, NULL, 1, 1))
            {
                THROW_HR(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED);
            }
        }
        else
        {
            memset(&m_document, 0, sizeof(m_document));
        }
    }

    Document::~Document()
    {
        if (m_token)
        {
            yaml_document_delete(&m_document);
        }
    }

    bool Document::HasRoot()
    {
        return yaml_document_get_root_node(&m_document) != nullptr;
    }

    Node Document::GetRoot()
    {
        yaml_node_t* root = yaml_document_get_root_node(&m_document);

        if (!root)
        {
            return {};
        }

        Node result(ConvertNodeType(root->type), ConvertYamlString(root->tag, root->start_mark), ConvertMark(root->start_mark));

        struct StackItem
        {
            StackItem(yaml_node_t* yn, Node* n) :
                yamlNode(yn), node(n) {}

            yaml_node_t* yamlNode = nullptr;
            Node* node = nullptr;
            size_t childOffset = 0;
        };

        static int YAML_DOCUMENT_NEST_LEVEL_LIMIT = 100;
        int nestLevel = 0;

        std::stack<StackItem> resultStack;
        resultStack.emplace(root, &result);

        while (!resultStack.empty())
        {
            StackItem& stackItem = resultStack.top();
            bool pop = false;

            switch (stackItem.yamlNode->type)
            {
            case YAML_NO_NODE:
                pop = true;
                break;
            case YAML_SCALAR_NODE:
                stackItem.node->SetScalar(
                    ConvertScalarToString(stackItem.yamlNode, stackItem.yamlNode->start_mark),
                    stackItem.yamlNode->data.scalar.style == YAML_SINGLE_QUOTED_SCALAR_STYLE ||
                    stackItem.yamlNode->data.scalar.style == YAML_DOUBLE_QUOTED_SCALAR_STYLE);
                pop = true;
                break;
            case YAML_SEQUENCE_NODE:
            {
                if (stackItem.childOffset == 0)
                {
                    // We've entered the sequence.
                    nestLevel++;
                }

                yaml_node_item_t* child = stackItem.yamlNode->data.sequence.items.start + stackItem.childOffset++;
                if (child < stackItem.yamlNode->data.sequence.items.top)
                {
                    yaml_node_t* childYamlNode = GetNode(*child);
                    Node& childNode = stackItem.node->AddSequenceNode(ConvertNodeType(childYamlNode->type), ConvertYamlString(childYamlNode->tag, childYamlNode->start_mark), ConvertMark(childYamlNode->start_mark));
                    resultStack.emplace(childYamlNode, &childNode);
                }
                else
                {
                    // We've reached the end of the sequence
                    pop = true;
                    nestLevel--;
                }
                break;
            }
            case YAML_MAPPING_NODE:
            {
                if (stackItem.childOffset == 0)
                {
                    // We've entered the mapping.
                    nestLevel++;
                }

                yaml_node_pair_t* child = stackItem.yamlNode->data.mapping.pairs.start + stackItem.childOffset++;
                if (child < stackItem.yamlNode->data.mapping.pairs.top)
                {
                    yaml_node_t* keyYamlNode = GetNode(child->key);
                    THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INVALID_MAPPING_KEY, keyYamlNode->type != YAML_SCALAR_NODE);

                    Node keyNode(ConvertNodeType(keyYamlNode->type), ConvertYamlString(keyYamlNode->tag, keyYamlNode->start_mark), ConvertMark(keyYamlNode->start_mark));
                    keyNode.SetScalar(ConvertScalarToString(keyYamlNode, keyYamlNode->start_mark));

                    yaml_node_t* valueYamlNode = GetNode(child->value);

                    Node& childNode = stackItem.node->AddMappingNode(std::move(keyNode), ConvertNodeType(valueYamlNode->type), ConvertYamlString(valueYamlNode->tag, valueYamlNode->start_mark), ConvertMark(valueYamlNode->start_mark));
                    resultStack.emplace(valueYamlNode, &childNode);
                }
                else
                {
                    // We've reached the end of the mapping
                    pop = true;
                    nestLevel--;
                }
                break;
            }
            }

            if (pop)
            {
                resultStack.pop();
            }

            THROW_HR_IF_MSG(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED, nestLevel > YAML_DOCUMENT_NEST_LEVEL_LIMIT, "Too many layers of nested nodes.");
        }

        return result;
    }

    int Document::AddScalar(std::string_view value, ScalarStyle style)
    {
        int result = yaml_document_add_scalar(&m_document, NULL, reinterpret_cast<const yaml_char_t*>(value.data()), static_cast<int>(value.size()), ConvertStyle(style));
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED, result == 0);
        return result;
    }

    int Document::AddSequence()
    {
        int result = yaml_document_add_sequence(&m_document, NULL, YAML_ANY_SEQUENCE_STYLE);
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED, result == 0);
        return result;
    }

    int Document::AddMapping()
    {
        int result = yaml_document_add_mapping(&m_document, NULL, YAML_ANY_MAPPING_STYLE);
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED, result == 0);
        return result;
    }

    void Document::AppendSequenceItem(int sequence, int item)
    {
        if (!yaml_document_append_sequence_item(&m_document, sequence, item))
        {
            THROW_HR(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED);
        }
    }

    void Document::AppendMappingPair(int mapping, int key, int value)
    {
        if (!yaml_document_append_mapping_pair(&m_document, mapping, key, value))
        {
            THROW_HR(APPINSTALLER_CLI_ERROR_YAML_DOC_BUILD_FAILED);
        }
    }

    yaml_node_t* Document::GetNode(yaml_node_item_t index)
    {
        yaml_node_t* result = yaml_document_get_node(&m_document, index);
        THROW_HR_IF(E_BOUNDS, !result);
        return result;
    }

    Parser::Parser(std::string_view input) : m_token(true), m_input(input)
    {
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_parser_initialize(&m_parser));

        PrepareInput();
        yaml_parser_set_input_string(&m_parser, reinterpret_cast<const unsigned char*>(m_input.c_str()), m_input.size());
    }

    Parser::Parser(std::istream& input, Utility::SHA256::HashBuffer* hashOut) : m_token(true)
    {
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_parser_initialize(&m_parser));

        m_input = Utility::ReadEntireStream(input);

        if (hashOut)
        {
            *hashOut = Utility::SHA256::ComputeHash(reinterpret_cast<const uint8_t*>(m_input.data()), static_cast<uint32_t>(m_input.size()));
        }

        PrepareInput();
        yaml_parser_set_input_string(&m_parser, reinterpret_cast<const unsigned char*>(m_input.c_str()), m_input.size());
    }

    Parser::~Parser()
    {
        if (m_token)
        {
            yaml_parser_delete(&m_parser);
        }
    }

    Document Parser::Load()
    {
        Document result;

        if (!yaml_parser_load(&m_parser, &result))
        {
            Exception::Type type = ConvertErrorType(m_parser.error);

            switch (type)
            {
            case Exception::Type::Memory:
                THROW_EXCEPTION(Exception(type));
            case Exception::Type::Reader:
                THROW_EXCEPTION(Exception(type, m_parser.problem, m_parser.problem_offset, m_parser.problem_value));
            case Exception::Type::Scanner:
            case Exception::Type::Parser:
            case Exception::Type::Composer:
                THROW_EXCEPTION(Exception(type, m_parser.problem, ConvertMark(m_parser.problem_mark), m_parser.context, ConvertMark(m_parser.context_mark)));
            default:
                THROW_EXCEPTION(Exception(type, "An unexpected error type occurred in Parser::Load"));
            }
        }

        return result;
    }

    void Parser::PrepareInput()
    {
        constexpr char c_utf16LEBOM[2] = { static_cast<char>(0xFF), static_cast<char>(0xFE) };
        constexpr char c_utf16BEBOM[2] = { static_cast<char>(0xFE), static_cast<char>(0xFF) };
        constexpr char c_utf8BOM[3] = { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) };

        // If input has a BOM, we want to remove it to prevent errors with checking for comments within the input document.

        // Check for UTF-16 BOMs
        if (m_input.size() >= sizeof(c_utf16LEBOM) && std::memcmp(m_input.data(), c_utf16LEBOM, sizeof(c_utf16LEBOM)) == 0)
        {
            AICLI_LOG(YAML, Verbose, << "Found UTF-16 LE BOM");
            yaml_parser_set_encoding(&m_parser, YAML_UTF16LE_ENCODING); // Without the BOM, the encoding must be explicitly set
            m_input.erase(0, sizeof(c_utf16LEBOM)); // Remove the BOM from the input
            return;
        }

        if (m_input.size() >= sizeof(c_utf16BEBOM) && std::memcmp(m_input.data(), c_utf16BEBOM, sizeof(c_utf16BEBOM)) == 0)
        {
            AICLI_LOG(YAML, Verbose, << "Found UTF-16 BE BOM");
            yaml_parser_set_encoding(&m_parser, YAML_UTF16BE_ENCODING); // Without the BOM, the encoding must be explicitly set
            m_input.erase(0, sizeof(c_utf16BEBOM)); // Remove the BOM from the input
            return;
        }

        // Check for UTF-8 BOM
        if (m_input.size() >= sizeof(c_utf8BOM) && std::memcmp(m_input.data(), c_utf8BOM, sizeof(c_utf8BOM)) == 0)
        {
            AICLI_LOG(YAML, Verbose, << "Found UTF-8 BOM");
            yaml_parser_set_encoding(&m_parser, YAML_UTF8_ENCODING); // Without the BOM, the encoding must be explicitly set
            m_input.erase(0, sizeof(c_utf8BOM)); // Remove the BOM from the input
            return;
        }

        // Check for BOM-less UTF-16 LE
        INT expectedTests = IS_TEXT_UNICODE_ASCII16 | IS_TEXT_UNICODE_STATISTICS | IS_TEXT_UNICODE_CONTROLS;
        INT testResults = expectedTests;
        if (IsTextUnicode(m_input.data(), wil::safe_cast<int>(m_input.size()), &testResults) || testResults == expectedTests)
        {
            AICLI_LOG(YAML, Verbose, << "Detected UTF-16 LE");
            yaml_parser_set_encoding(&m_parser, YAML_UTF16LE_ENCODING);
            return;
        }

        // Check for BOM-less UTF-16 BE
        expectedTests = IS_TEXT_UNICODE_REVERSE_ASCII16 | IS_TEXT_UNICODE_REVERSE_STATISTICS | IS_TEXT_UNICODE_REVERSE_CONTROLS;
        testResults = expectedTests;
        if (IsTextUnicode(m_input.data(), wil::safe_cast<int>(m_input.size()), &testResults) || testResults == expectedTests)
        {
            AICLI_LOG(YAML, Verbose, << "Detected UTF-16 BE");
            yaml_parser_set_encoding(&m_parser, YAML_UTF16BE_ENCODING);
            return;
        }

        // Check for BOM-less UTF-8
        UINT nChars = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            m_input.data(),
            wil::safe_cast<int>(m_input.size()),
            NULL,
            0);

        if (nChars > 0 || GetLastError() != ERROR_NO_UNICODE_TRANSLATION)
        {
            AICLI_LOG(YAML, Verbose, << "Detected UTF-8");
            yaml_parser_set_encoding(&m_parser, YAML_UTF8_ENCODING);
            return;
        }

        // Must be ANSI (Windows-1252 assumed), convert to UTF-8
        AICLI_LOG(YAML, Verbose, << "Assuming ANSI Windows-1252");
        std::wstring utf16 = Utility::ConvertToUTF16(m_input, 1252);
        m_input = Utility::ConvertToUTF8(utf16);
        yaml_parser_set_encoding(&m_parser, YAML_UTF8_ENCODING);
    }

    Event::~Event()
    {
        if (m_token)
        {
            yaml_event_delete(&m_event);
        }
    }

    Event Event::StreamStart()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_stream_start_event_initialize(&result, YAML_UTF8_ENCODING));
        result.m_token = true;
        return result;
    }

    Event Event::StreamEnd()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_stream_end_event_initialize(&result));
        result.m_token = true;
        return result;
    }

    Event Event::DocumentStart()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_document_start_event_initialize(&result, NULL, NULL, NULL, 1));
        result.m_token = true;
        return result;
    }

    Event Event::DocumentEnd()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_document_end_event_initialize(&result, 1));
        result.m_token = true;
        return result;
    }

    Event Event::SequenceStart()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_sequence_start_event_initialize(&result, NULL, NULL, 1, YAML_ANY_SEQUENCE_STYLE));
        result.m_token = true;
        return result;
    }

    Event Event::SequenceEnd()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_sequence_end_event_initialize(&result));
        result.m_token = true;
        return result;
    }

    Event Event::MappingStart()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_mapping_start_event_initialize(&result, NULL, NULL, 1, YAML_ANY_MAPPING_STYLE));
        result.m_token = true;
        return result;
    }

    Event Event::MappingEnd()
    {
        Event result;
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_mapping_end_event_initialize(&result));
        result.m_token = true;
        return result;
    }

    Emitter::Emitter(std::ostream& output) :
        m_token(true), m_outputStream(&output)
    {
        THROW_HR_IF(APPINSTALLER_CLI_ERROR_YAML_INIT_FAILED, !yaml_emitter_initialize(&m_emitter));
        yaml_emitter_set_output(&m_emitter, StreamWriteHandler, this);
        yaml_emitter_set_encoding(&m_emitter, YAML_UTF8_ENCODING);
    }

    Emitter::~Emitter()
    {
        if (m_token)
        {
            yaml_emitter_delete(&m_emitter);
        }
    }

    void Emitter::Emit(Event& event)
    {
        event.Detach();
        if (!yaml_emitter_emit(&m_emitter, &event))
        {
            ThrowError();
        }
    }

    void Emitter::Emit(Event&& event)
    {
        event.Detach();
        if (!yaml_emitter_emit(&m_emitter, &event))
        {
            ThrowError();
        }
    }

    void Emitter::Dump(Document& document)
    {
        document.Detach();
        if (!yaml_emitter_dump(&m_emitter, &document))
        {
            ThrowError();
        }
    }

    void Emitter::Flush()
    {
        if (!yaml_emitter_flush(&m_emitter))
        {
            ThrowError();
        }
    }

    int Emitter::StreamWriteHandler(
        void* data,
        unsigned char* buffer,
        size_t size)
    {
        Emitter& emitter = *reinterpret_cast<Emitter*>(data);

        try
        {
            emitter.m_outputStream->write(reinterpret_cast<char*>(buffer), size);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            return 0;
        }

        return 1;
    }

    void Emitter::ThrowError()
    {
        Exception::Type type = ConvertErrorType(m_emitter.error);

        switch (type)
        {
        case Exception::Type::Memory:
            THROW_EXCEPTION(Exception(type));
        case Exception::Type::Emitter:
        case Exception::Type::Writer:
            THROW_EXCEPTION(Exception(type, m_emitter.problem));
        default:
            THROW_EXCEPTION(Exception(type, "An unexpected error type occurred in Emitter"));
        }
    }
}
