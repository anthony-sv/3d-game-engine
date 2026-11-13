#include "Editor/Agents/AgentKit.h"

#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Hash.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <map>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view AgentsPath = "AGENTS.md";
		constexpr std::string_view ClaudePath = "CLAUDE.md";
		constexpr std::string_view McpPath = ".mcp.json";
		constexpr std::string_view SkillPath = ".claude/skills/strada-make-game/SKILL.md";
		constexpr std::string_view SkillFileName = "SKILL.md";
		constexpr std::string_view GuidesDirectory = ".strada/docs/";
		constexpr std::string_view GuidesReadmePath = ".strada/docs/README.md";
		constexpr std::string_view GitIgnorePath = ".gitignore";
		constexpr std::string_view ClaudeLine = "@AGENTS.md";
		constexpr std::string_view GitIgnoreLine = ".strada/";
		// The root .gitignore of a new project (spec §4.1), written when a setup finds none.
		constexpr std::string_view GitIgnoreText = "# Strada: build output, caches and machine-specific files.\n"
												   "Build/\n"
												   ".strada/\n"
												   "Scripts/Binaries/\n"
												   "Scripts/bin/\n"
												   "Scripts/obj/\n"
												   "Scripts/Strada.props\n";
		constexpr std::string_view GuidesReadmeText =
			"# Strada's guides\n"
			"\n"
			"Strada manages this folder: it holds copies of the engine's guides for coding agents. Setting the project up for\n"
			"coding agents again replaces them with the engine's (the editor's File > Set Up for Coding Agents..., or the\n"
			"`project_setup_agents` tool), so do not edit them here. Git ignores `.strada/`.\n"
			"\n"
			"The guides link to each other and to the game-making skill where they lie in this project. A link to a file that is\n"
			"not copied here names that file's path in the engine's repository instead.\n";
		// A UTF-8 byte order mark, which a setup keeps at the start of a file it adds to.
		constexpr std::string_view ByteOrderMark = "\xEF\xBB\xBF";
		// Lines of context around each change of a diff.
		constexpr size_t DiffContext = 3;
		// Above this many cells the middle of a diff is shown as removed and added instead of matched line by line.
		constexpr size_t MaxDiffCells = 250000;

		// How the kit judges a file.
		enum class FileKind : uint8_t
		{
			AgentsBlock = 0,
			ClaudeLine,
			McpEntry,
			// The skill: whole, skipped when the user changed it.
			WholeFile,
			// A guide under .strada/docs/: whole, always replaced.
			Guide,
			GitIgnore,
			State
		};

		// A file of the project as the kit found it.
		struct FoundFile
		{
			bool Exists = false;
			// Why it cannot be read as a file of the kit; empty when it can (or does not exist).
			std::string Problem;
			// The text without its UTF-8 byte order mark, and whether it had one.
			std::string Text;
			bool HasByteOrderMark = false;
		};

		// A file of the kit, judged.
		struct Entry
		{
			std::string Path;
			FileKind Kind = FileKind::WholeFile;
			AgentFileState State = AgentFileState::Missing;
			AgentFileAction Action = AgentFileAction::Unchanged;
			// The whole file to write when the action creates or updates it.
			std::string NewText;
			// The managed part before and after (line endings removed), for the diff.
			std::string OldPart;
			std::string NewPart;
			// Whole files: the hash of the engine's text, which the state file records once the kit wrote it.
			std::string Hash;
			// Why a Damaged file cannot be judged, when the file itself could be read.
			std::string Problem;
		};

		struct Evaluation
		{
			std::vector<Entry> Entries;
			std::string Tool;
			std::vector<std::string> Warnings;
			// The state file's records as loaded (empty for a missing, damaged or newer one).
			std::map<std::string, std::string> Records;
		};

		std::string RemoveCarriageReturns(std::string_view text)
		{
			std::string result(text);
			std::erase(result, '\r');
			return result;
		}

		// 16 lowercase hex digits of the FNV-1a hash of the text without carriage returns: a checkout that converts line
		// endings keeps a file's hash.
		std::string HashText(std::string_view text)
		{
			return fmt::format("{:016x}", Hash::FNV1a(RemoveCarriageReturns(text)));
		}

		bool IsRecordedHash(std::string_view text)
		{
			return text.size() == 16 && std::ranges::all_of(text,
			                                                [](char character)
			                                                {
																return (character >= '0' && character <= '9') ||
				                                                       (character >= 'a' && character <= 'f');
															});
		}

		// The line ending of a file: the one its first line ends with, "\n" when it has none.
		std::string_view DetectLineEnding(std::string_view text)
		{
			size_t const newline = text.find('\n');
			return newline != std::string_view::npos && newline > 0 && text[newline - 1] == '\r' ? "\r\n" : "\n";
		}

		std::string WithLineEnding(std::string_view text, std::string_view lineEnding)
		{
			std::string result;
			result.reserve(text.size());
			for (char const character : text)
			{
				if (character == '\n')
				{
					result += lineEnding;
				}
				else
				{
					result += character;
				}
			}
			return result;
		}

		std::string_view Trim(std::string_view text)
		{
			constexpr std::string_view Blank = " \t\r";
			size_t const first = text.find_first_not_of(Blank);
			if (first == std::string_view::npos)
			{
				return {};
			}
			size_t const last = text.find_last_not_of(Blank);
			return text.substr(first, last - first + 1);
		}

		struct LineSpan
		{
			// Where the line starts, where its text ends (before "\r\n" or "\n") and where the next line starts.
			size_t Begin = 0;
			size_t ContentEnd = 0;
			size_t End = 0;
		};

		std::vector<LineSpan> SplitLines(std::string_view text)
		{
			std::vector<LineSpan> lines;
			size_t begin = 0;
			while (begin < text.size())
			{
				size_t const newline = text.find('\n', begin);
				LineSpan line;
				line.Begin = begin;
				if (newline == std::string_view::npos)
				{
					line.ContentEnd = text.size();
					line.End = text.size();
				}
				else
				{
					line.ContentEnd = newline > begin && text[newline - 1] == '\r' ? newline - 1 : newline;
					line.End = newline + 1;
				}
				lines.push_back(line);
				begin = line.End;
			}
			return lines;
		}

		bool HasLine(std::string_view text, std::initializer_list<std::string_view> accepted)
		{
			for (LineSpan const& line : SplitLines(text))
			{
				std::string_view const content = Trim(text.substr(line.Begin, line.ContentEnd - line.Begin));
				if (std::ranges::find(accepted, content) != accepted.end())
				{
					return true;
				}
			}
			return false;
		}

		// The text with a line appended: after a line ending when the text does not end with one.
		std::string AppendLine(std::string_view text, std::string_view line)
		{
			std::string_view const lineEnding = DetectLineEnding(text);
			std::string result(text);
			if (!result.empty() && result.back() != '\n')
			{
				result += lineEnding;
			}
			result += line;
			result += lineEnding;
			return result;
		}

		// A project file below the project's directory, never through a symbolic link.
		FoundFile ReadProjectFile(std::filesystem::path const& projectDirectory, std::string_view relative)
		{
			FoundFile found;
			std::filesystem::path current = projectDirectory;
			std::filesystem::path const relativePath = FileSystem::PathFromUtf8(relative);
			size_t const count = static_cast<size_t>(std::distance(relativePath.begin(), relativePath.end()));
			size_t position = 0;
			for (std::filesystem::path const& name : relativePath)
			{
				bool const isLast = ++position == count;
				current /= name;
				std::error_code error;
				std::filesystem::file_status const status = std::filesystem::symlink_status(current, error);
				if (status.type() == std::filesystem::file_type::not_found)
				{
					// The file, or a folder on its way, does not exist.
					return FoundFile{};
				}
				found.Exists = true;
				if (error)
				{
					found.Problem = fmt::format("'{}' cannot be read: {}", FileSystem::PathToUtf8(current), error.message());
					return found;
				}
				if (std::filesystem::is_symlink(status))
				{
					found.Problem = fmt::format("'{}' is a symbolic link, which Strada does not follow",
					                            FileSystem::PathToUtf8(current.lexically_relative(projectDirectory)));
					return found;
				}
				if (!isLast && !std::filesystem::is_directory(status))
				{
					found.Problem =
						fmt::format("'{}' is not a folder", FileSystem::PathToUtf8(current.lexically_relative(projectDirectory)));
					return found;
				}
				if (isLast && !std::filesystem::is_regular_file(status))
				{
					found.Problem = "it is not a file";
					return found;
				}
			}
			std::optional<uint64_t> const size = FileSystem::GetFileSize(current);
			if (!size || *size > AgentKit::MaxFileBytes)
			{
				found.Problem = size ? fmt::format("it is larger than {} bytes", AgentKit::MaxFileBytes) : "its size cannot be read";
				return found;
			}
			// As bytes: FileSystem::ReadTextFile drops a byte order mark, which a rewrite must put back.
			Result<Buffer> bytes = FileSystem::ReadBinaryFile(current);
			if (!bytes)
			{
				found.Problem = bytes.GetError();
				return found;
			}
			Buffer const& buffer = bytes.GetValue();
			found.Text.assign(reinterpret_cast<char const*>(buffer.GetData()), static_cast<size_t>(buffer.GetSize()));
			if (found.Text.starts_with(ByteOrderMark))
			{
				found.HasByteOrderMark = true;
				found.Text.erase(0, ByteOrderMark.size());
			}
			return found;
		}

		// A file of the engine's content; nothing when it is missing, too large or unreadable.
		std::optional<std::string> ReadEngineFile(std::filesystem::path const& path)
		{
			if (path.empty() || !FileSystem::IsRegularFile(path))
			{
				return std::nullopt;
			}
			std::optional<uint64_t> const size = FileSystem::GetFileSize(path);
			if (!size || *size > AgentKit::MaxFileBytes)
			{
				return std::nullopt;
			}
			Result<std::string> text = FileSystem::ReadTextFile(path);
			if (!text)
			{
				return std::nullopt;
			}
			return text.TakeValue();
		}

		// Control characters of a name would break the block's lines: they become spaces.
		std::string MakeSingleLine(std::string_view value)
		{
			std::string result(value);
			for (char& character : result)
			{
				if (static_cast<unsigned char>(character) < 0x20 || character == 0x7F)
				{
					character = ' ';
				}
			}
			return result;
		}

		// The engine's block with the placeholders filled in (in one pass, so a name that looks like a placeholder stays as
		// it is), without carriage returns, ending with a line ending.
		std::string FillBlock(std::string_view block, AgentProject const& project)
		{
			constexpr std::string_view NamePlaceholder = "{{ProjectName}}";
			constexpr std::string_view FilePlaceholder = "{{ProjectFile}}";
			std::string const name = MakeSingleLine(project.Name);
			std::string const file = MakeSingleLine(project.ProjectFile);
			std::string const text = RemoveCarriageReturns(block);
			std::string result;
			size_t position = 0;
			while (position < text.size())
			{
				std::string_view const rest = std::string_view(text).substr(position);
				if (rest.starts_with(NamePlaceholder))
				{
					result += name;
					position += NamePlaceholder.size();
				}
				else if (rest.starts_with(FilePlaceholder))
				{
					result += file;
					position += FilePlaceholder.size();
				}
				else
				{
					result += text[position];
					position++;
				}
			}
			if (!result.empty() && result.back() != '\n')
			{
				result += '\n';
			}
			return result;
		}

		// The length of the run of this character that starts at position.
		size_t CountRun(std::string_view text, size_t position, char character)
		{
			size_t const end = text.find_first_not_of(character, position);
			return (end == std::string_view::npos ? text.size() : end) - position;
		}

		// Where a run of exactly length backticks starts at or after from (what closes a code span); npos when none does.
		size_t FindBacktickRun(std::string_view line, size_t from, size_t length)
		{
			size_t index = from;
			while (index < line.size())
			{
				if (line[index] != '`')
				{
					index++;
					continue;
				}
				size_t const run = CountRun(line, index, '`');
				if (run == length)
				{
					return index;
				}
				index += run;
			}
			return std::string_view::npos;
		}

		// A guide's relative link target resolved against the engine repository's Docs/ folder ("../AGENTS.md" is
		// "AGENTS.md"); empty when it leads out of the repository.
		std::string ResolveRepositoryPath(std::string_view target)
		{
			std::vector<std::string_view> parts = {"Docs"};
			size_t position = 0;
			while (position <= target.size())
			{
				size_t const slash = std::min(target.find('/', position), target.size());
				std::string_view const part = target.substr(position, slash - position);
				if (part == "..")
				{
					if (parts.empty())
					{
						return {};
					}
					parts.pop_back();
				}
				else if (!part.empty() && part != ".")
				{
					parts.push_back(part);
				}
				position = slash + 1;
			}
			std::string path;
			for (std::string_view const part : parts)
			{
				if (!path.empty())
				{
					path += '/';
				}
				path += part;
			}
			return path;
		}

		// A link of a guide, [text](target), as its copy under .strada/docs/ shows it: a link to a copied guide or to the
		// skill points where that file lies in the project; a link to another file of the repository becomes its text and the
		// file's path in the engine's repository. Links to anchors and to other sites stay as they are.
		std::string RewriteGuideLink(std::string_view text, std::string_view target)
		{
			if (target.starts_with('#') || target.find(':') != std::string_view::npos)
			{
				return fmt::format("[{}]({})", text, target);
			}
			size_t const anchorStart = target.find('#');
			std::string_view const path = target.substr(0, anchorStart);
			std::string_view const anchor = anchorStart == std::string_view::npos ? std::string_view() : target.substr(anchorStart);
			std::string const repositoryPath = ResolveRepositoryPath(path);
			if (repositoryPath.empty())
			{
				return std::string(text);
			}
			for (std::string_view const guide : AgentKit::Guides)
			{
				if (repositoryPath == fmt::format("Docs/{}", guide))
				{
					return fmt::format("[{}]({}{})", text, guide, anchor);
				}
			}
			if (repositoryPath == SkillPath)
			{
				// From .strada/docs/ up to the project's directory.
				return fmt::format("[{}](../../{}{})", text, SkillPath, anchor);
			}
			return fmt::format("{} (`{}` in the engine's repository)", text, repositoryPath);
		}

		// Where the text of a link that opens at position ends (its closing bracket), skipping code spans and nested
		// brackets; npos when the line does not close it.
		size_t FindLinkTextEnd(std::string_view line, size_t position)
		{
			size_t depth = 0;
			for (size_t index = position; index < line.size(); index++)
			{
				if (line[index] == '`')
				{
					size_t const run = CountRun(line, index, '`');
					size_t const close = FindBacktickRun(line, index + run, run);
					if (close == std::string_view::npos)
					{
						return std::string_view::npos;
					}
					index = close + run - 1;
				}
				else if (line[index] == '[')
				{
					depth++;
				}
				else if (line[index] == ']' && --depth == 0)
				{
					return index;
				}
			}
			return std::string_view::npos;
		}

		// The links of one line outside code spans, rewritten (RewriteGuideLink). Images and escaped brackets stay as they
		// are, and so do targets with a title, spaces or brackets, which no guide links a file with.
		std::string RewriteLineLinks(std::string_view line)
		{
			std::string result;
			result.reserve(line.size());
			size_t position = 0;
			while (position < line.size())
			{
				char const character = line[position];
				if (character == '`')
				{
					size_t const run = CountRun(line, position, '`');
					size_t const close = FindBacktickRun(line, position + run, run);
					size_t const end = close == std::string_view::npos ? position + run : close + run;
					result += line.substr(position, end - position);
					position = end;
					continue;
				}
				bool const opensLink = character == '[' && (position == 0 || (line[position - 1] != '!' && line[position - 1] != '\\'));
				size_t const textEnd = opensLink ? FindLinkTextEnd(line, position) : std::string_view::npos;
				if (textEnd != std::string_view::npos && textEnd + 1 < line.size() && line[textEnd + 1] == '(')
				{
					size_t const targetBegin = textEnd + 2;
					size_t const targetEnd = line.find(')', targetBegin);
					if (targetEnd != std::string_view::npos && targetEnd > targetBegin)
					{
						std::string_view const target = line.substr(targetBegin, targetEnd - targetBegin);
						if (target.find_first_of(" \t(<\"") == std::string_view::npos)
						{
							result += RewriteGuideLink(line.substr(position + 1, textEnd - position - 1), target);
							position = targetEnd + 1;
							continue;
						}
					}
				}
				result += character;
				position++;
			}
			return result;
		}

		// A guide as a setup copies it to .strada/docs/: its links outside code adapted to that folder (RewriteGuideLink),
		// every other byte as it is.
		std::string RewriteGuideLinks(std::string_view text)
		{
			std::string result;
			result.reserve(text.size());
			// The fence that opened the fenced code block the line is in ("```", "~~~~"); empty outside one.
			std::string_view fence;
			for (LineSpan const& line : SplitLines(text))
			{
				std::string_view const content = text.substr(line.Begin, line.ContentEnd - line.Begin);
				std::string_view const lineEnding = text.substr(line.ContentEnd, line.End - line.ContentEnd);
				std::string_view const indented = content.substr(std::min(content.find_first_not_of(' '), content.size()));
				if (!fence.empty())
				{
					if (indented.starts_with(fence))
					{
						fence = {};
					}
					result += content;
				}
				else if (indented.starts_with("```") || indented.starts_with("~~~"))
				{
					fence = indented.substr(0, CountRun(indented, 0, indented.front()));
					result += content;
				}
				else
				{
					result += RewriteLineLinks(content);
				}
				result += lineEnding;
			}
			return result;
		}

		bool IsMarkerLine(std::string_view content)
		{
			std::string_view const trimmed = Trim(content);
			return trimmed == AgentKit::BeginMarker || trimmed == AgentKit::EndMarker;
		}

		// AGENTS.md: the block between the markers. block is the filled block (LF line endings).
		void JudgeAgents(Entry& entry, FoundFile const& file, std::string const& block)
		{
			std::string const managed = fmt::format("{}\n{}{}\n", AgentKit::BeginMarker, block, AgentKit::EndMarker);
			entry.NewPart = managed;
			if (!file.Exists)
			{
				entry.State = AgentFileState::Missing;
				entry.NewText = managed;
				return;
			}
			std::string_view const text = file.Text;
			std::vector<LineSpan> const lines = SplitLines(text);
			std::vector<size_t> begins;
			std::vector<size_t> ends;
			for (size_t index = 0; index < lines.size(); index++)
			{
				std::string_view const content = Trim(text.substr(lines[index].Begin, lines[index].ContentEnd - lines[index].Begin));
				if (content == AgentKit::BeginMarker)
				{
					begins.push_back(index);
				}
				else if (content == AgentKit::EndMarker)
				{
					ends.push_back(index);
				}
			}
			std::string_view const lineEnding = DetectLineEnding(text);
			if (begins.empty() && ends.empty())
			{
				entry.State = AgentFileState::Outdated;
				std::string updated(text);
				if (!updated.empty())
				{
					if (updated.back() != '\n')
					{
						updated += lineEnding;
					}
					// One blank line between the user's text and the block.
					bool const endsWithBlankLine =
						updated == "\n" || updated == "\r\n" || updated.ends_with("\n\n") || updated.ends_with("\n\r\n");
					if (!endsWithBlankLine)
					{
						updated += lineEnding;
					}
				}
				updated += WithLineEnding(managed, lineEnding);
				entry.NewText = std::move(updated);
				return;
			}
			if (begins.size() != 1 || ends.size() != 1 || begins[0] > ends[0])
			{
				entry.State = AgentFileState::Damaged;
				entry.Problem =
					fmt::format("its '{}' and '{}' lines are not one pair in this order", AgentKit::BeginMarker, AgentKit::EndMarker);
				return;
			}
			LineSpan const& begin = lines[begins[0]];
			LineSpan const& end = lines[ends[0]];
			std::string const inner = RemoveCarriageReturns(text.substr(begin.End, end.Begin - begin.End));
			entry.OldPart = fmt::format("{}\n{}{}\n", AgentKit::BeginMarker, inner, AgentKit::EndMarker);
			if (inner == block)
			{
				entry.State = AgentFileState::Current;
				return;
			}
			entry.State = AgentFileState::Outdated;
			// The text before the BEGIN line and from the END line's own line ending on stays byte for byte.
			std::string const replaced = WithLineEnding(std::string_view(managed).substr(0, managed.size() - 1), lineEnding);
			entry.NewText = std::string(text.substr(0, begin.Begin)) + replaced + std::string(text.substr(end.ContentEnd));
		}

		// CLAUDE.md and .gitignore: one line the file must hold.
		void JudgeLine(Entry& entry, FoundFile const& file, std::string_view line, std::initializer_list<std::string_view> accepted,
		               std::string_view newFileText)
		{
			entry.NewPart = fmt::format("{}\n", line);
			if (!file.Exists)
			{
				entry.State = AgentFileState::Missing;
				entry.NewText = std::string(newFileText);
				entry.NewPart = std::string(newFileText);
				return;
			}
			if (HasLine(file.Text, accepted))
			{
				entry.State = AgentFileState::Current;
				entry.OldPart = entry.NewPart;
				return;
			}
			entry.State = AgentFileState::Outdated;
			entry.NewText = AppendLine(file.Text, line);
		}

		struct JsonStyle
		{
			// nlohmann's indent: -1 writes the document on one line.
			int Indent = 1;
			char Character = '\t';
		};

		// The indentation of a JSON file: that of its first indented line; one line when it has none.
		JsonStyle DetectJsonStyle(std::string_view text)
		{
			for (LineSpan const& line : SplitLines(text))
			{
				std::string_view const content = text.substr(line.Begin, line.ContentEnd - line.Begin);
				if (content.empty() || Trim(content).empty())
				{
					continue;
				}
				if (content.front() == '\t')
				{
					return JsonStyle{1, '\t'};
				}
				if (content.front() == ' ')
				{
					size_t const spaces = std::min<size_t>(content.find_first_not_of(' '), 16);
					return JsonStyle{static_cast<int>(spaces), ' '};
				}
			}
			return JsonStyle{-1, ' '};
		}

		std::string DumpJsonWithStyle(Json const& json, JsonStyle style)
		{
			return json.dump(style.Indent, style.Character, false, Json::error_handler_t::replace);
		}

		bool IsKitEntry(Json const& entry, Json const& wanted)
		{
			if (!entry.is_object() || entry.size() != wanted.size())
			{
				return false;
			}
			for (auto const& [key, value] : wanted.items())
			{
				auto const member = entry.find(key);
				if (member == entry.end() || *member != value)
				{
					return false;
				}
			}
			return true;
		}

		// .mcp.json: the strada entry of mcpServers, merged into the file with everything else kept.
		void JudgeMcp(Entry& entry, FoundFile const& file, std::string const& tool, std::string const& projectFile)
		{
			Json const wanted = Json::object({{"command", tool}, {"args", Json::array({"mcp", "--project", projectFile})}});
			if (!file.Exists)
			{
				entry.State = AgentFileState::Missing;
				entry.NewText = DumpJson(Json::object({{"mcpServers", Json::object({{"strada", wanted}})}}));
				entry.NewPart = DumpJsonWithStyle(wanted, JsonStyle{}) + "\n";
				return;
			}
			// An empty file is no JSON object either: Damaged, as every file that is not one.
			Result<Json> parsed = ParseJson(file.Text);
			if (!parsed)
			{
				entry.State = AgentFileState::Damaged;
				entry.Problem = parsed.GetError();
				return;
			}
			Json document = parsed.TakeValue();
			if (!document.is_object())
			{
				entry.State = AgentFileState::Damaged;
				entry.Problem = "it is not a JSON object";
				return;
			}
			JsonStyle style = DetectJsonStyle(file.Text);
			if (style.Indent < 0 && document.empty())
			{
				style = JsonStyle{};
			}
			auto const servers = document.find("mcpServers");
			if (servers != document.end() && !servers->is_object())
			{
				entry.State = AgentFileState::Damaged;
				entry.Problem = "its \"mcpServers\" is not a JSON object";
				return;
			}
			entry.NewPart = DumpJsonWithStyle(wanted, style) + "\n";
			if (servers != document.end())
			{
				auto const current = servers->find("strada");
				if (current != servers->end())
				{
					entry.OldPart = DumpJsonWithStyle(*current, style) + "\n";
					if (IsKitEntry(*current, wanted))
					{
						entry.State = AgentFileState::Current;
						return;
					}
				}
			}
			entry.State = AgentFileState::Outdated;
			if (servers == document.end())
			{
				document["mcpServers"] = Json::object();
			}
			document["mcpServers"]["strada"] = wanted;
			entry.NewText = WithLineEnding(DumpJsonWithStyle(document, style) + "\n", DetectLineEnding(file.Text));
		}

		// The skill and the guides: whole files, the engine's text.
		void JudgeWholeFile(Entry& entry, FoundFile const& file, std::string const& source,
		                    std::map<std::string, std::string> const& records)
		{
			entry.Hash = HashText(source);
			entry.NewPart = RemoveCarriageReturns(source);
			entry.NewText = source;
			if (!file.Exists)
			{
				entry.State = AgentFileState::Missing;
				return;
			}
			entry.OldPart = RemoveCarriageReturns(file.Text);
			if (entry.OldPart == entry.NewPart)
			{
				entry.State = AgentFileState::Current;
				return;
			}
			auto const record = records.find(entry.Path);
			entry.State =
				record != records.end() && record->second == HashText(file.Text) ? AgentFileState::Outdated : AgentFileState::UserModified;
		}

		std::map<std::string, std::string> LoadRecords(FoundFile const& file)
		{
			std::map<std::string, std::string> records;
			if (!file.Exists || !file.Problem.empty())
			{
				return records;
			}
			Result<Json> parsed = ParseJson(file.Text);
			if (!parsed || !ReadFileHeader(parsed.GetValue(), AgentKit::StateFileType, AgentKit::StateFileVersion))
			{
				return records;
			}
			Json const& document = parsed.GetValue();
			auto const files = document.find("Files");
			if (files == document.end() || !files->is_object())
			{
				return records;
			}
			for (auto const& [path, hash] : files->items())
			{
				if (hash.is_string() && IsRecordedHash(hash.get_ref<std::string const&>()))
				{
					records.emplace(path, hash.get<std::string>());
				}
			}
			return records;
		}

		AgentFileAction GetAction(Entry const& entry)
		{
			switch (entry.State)
			{
				case AgentFileState::Missing:
					return AgentFileAction::Create;
				case AgentFileState::Current:
					return AgentFileAction::Unchanged;
				case AgentFileState::Outdated:
					return AgentFileAction::Update;
				case AgentFileState::UserModified:
					// .strada/docs/ is Strada's folder.
					return entry.Kind == FileKind::Guide ? AgentFileAction::Update : AgentFileAction::SkipUserModified;
				case AgentFileState::Damaged:
					return AgentFileAction::SkipDamaged;
			}
			return AgentFileAction::SkipDamaged;
		}

		bool Writes(AgentFileAction action)
		{
			return action == AgentFileAction::Create || action == AgentFileAction::Update;
		}

		// The text of the state file: the hashes of the whole files, those the setup writes (or finds current) as the
		// engine's, the others as recorded before. written tells whether an entry's file was written (or would be).
		template<typename TWritten>
		std::string MakeStateText(Evaluation const& evaluation, TWritten const& written)
		{
			Json files = Json::object();
			for (Entry const& entry : evaluation.Entries)
			{
				if (entry.Kind != FileKind::WholeFile && entry.Kind != FileKind::Guide)
				{
					continue;
				}
				if (entry.Action == AgentFileAction::Unchanged || (Writes(entry.Action) && written(entry)))
				{
					files[entry.Path] = entry.Hash;
				}
				else if (auto const record = evaluation.Records.find(entry.Path); record != evaluation.Records.end())
				{
					files[entry.Path] = record->second;
				}
			}
			Json document = Json::object();
			document["Strada"] = MakeFileHeader(AgentKit::StateFileType, AgentKit::StateFileVersion);
			document["Files"] = std::move(files);
			return DumpJson(document);
		}

		void JudgeState(Entry& entry, FoundFile const& file, std::string const& text)
		{
			entry.NewText = text;
			entry.NewPart = text;
			if (!file.Exists)
			{
				entry.State = AgentFileState::Missing;
				return;
			}
			if (!file.Problem.empty())
			{
				entry.State = AgentFileState::Damaged;
				return;
			}
			entry.OldPart = RemoveCarriageReturns(file.Text);
			// A damaged or newer state file counts as empty, and the kit replaces it: the file is Strada's.
			entry.State = entry.OldPart == text ? AgentFileState::Current : AgentFileState::Outdated;
		}

		std::string DescribeSkipped(Entry const& entry, FoundFile const& file)
		{
			if (entry.State == AgentFileState::UserModified)
			{
				return fmt::format("'{}' was changed since Strada wrote it (or Strada never wrote it) and is left as it is: delete it "
				                   "and set up again to get the engine's",
				                   entry.Path);
			}
			return fmt::format("'{}' is left as it is: {}", entry.Path, file.Problem.empty() ? entry.Problem : file.Problem);
		}

		// The kinds of files a setup adds to rather than replaces: their byte order mark lies outside the managed part.
		bool KeepsByteOrderMark(FileKind kind)
		{
			return kind == FileKind::AgentsBlock || kind == FileKind::ClaudeLine || kind == FileKind::McpEntry ||
			       kind == FileKind::GitIgnore;
		}

		Result<Evaluation> Evaluate(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool,
		                            AgentSetupOptions const& options)
		{
			std::filesystem::path const kit = layout.GetContentPath(EngineContent::AgentKit);
			std::filesystem::path const skillDirectory = layout.GetContentPath(EngineContent::MakeGameSkill);
			if (kit.empty() || skillDirectory.empty())
			{
				return Error{"the editor has no engine content, so it has no agent kit"};
			}
			std::filesystem::path const blockPath = kit / FileSystem::PathFromUtf8(AgentKit::BlockFileName);
			std::optional<std::string> const blockSource = ReadEngineFile(blockPath);
			if (!blockSource)
			{
				return MakeError("the engine's agent kit is incomplete: '{}' cannot be read", FileSystem::PathToUtf8(blockPath));
			}
			std::filesystem::path const skillPath = skillDirectory / FileSystem::PathFromUtf8(SkillFileName);
			std::optional<std::string> const skill = ReadEngineFile(skillPath);
			if (!skill)
			{
				return MakeError("the engine's agent kit is incomplete: '{}' cannot be read", FileSystem::PathToUtf8(skillPath));
			}
			std::string const block = FillBlock(*blockSource, project);
			for (LineSpan const& line : SplitLines(block))
			{
				if (IsMarkerLine(std::string_view(block).substr(line.Begin, line.ContentEnd - line.Begin)))
				{
					return MakeError("the engine's agent block '{}' holds a strada-agents marker line", FileSystem::PathToUtf8(blockPath));
				}
			}

			Evaluation evaluation;
			evaluation.Tool = AgentKit::ResolveTool(layout, findTool, &evaluation.Warnings);
			FoundFile const stateFile = ReadProjectFile(project.Directory, AgentKit::StateFilePath);
			evaluation.Records = LoadRecords(stateFile);

			std::vector<FoundFile> found;
			// Room for every entry, so that the references add returns stay valid.
			evaluation.Entries.reserve(AgentKit::Guides.size() + 8);
			auto const add = [&evaluation, &found, &project](std::string_view path, FileKind kind) -> Entry&
			{
				Entry& added = evaluation.Entries.emplace_back();
				added.Path = std::string(path);
				added.Kind = kind;
				found.push_back(ReadProjectFile(project.Directory, path));
				return added;
			};
			// Each judge runs only on a file that can be read; one that cannot is Damaged.
			auto const judge = [&found](Entry& judged, auto const& judgeFile)
			{
				FoundFile const& read = found.back();
				if (read.Exists && !read.Problem.empty())
				{
					judged.State = AgentFileState::Damaged;
					return;
				}
				judgeFile(read);
				if (read.HasByteOrderMark && !judged.NewText.empty() && KeepsByteOrderMark(judged.Kind))
				{
					judged.NewText.insert(0, ByteOrderMark);
				}
			};

			Entry& agents = add(AgentsPath, FileKind::AgentsBlock);
			judge(agents,
			      [&agents, &block](FoundFile const& file)
			      {
					  JudgeAgents(agents, file, block);
				  });
			Entry& claude = add(ClaudePath, FileKind::ClaudeLine);
			judge(claude,
			      [&claude](FoundFile const& file)
			      {
					  JudgeLine(claude, file, ClaudeLine, {ClaudeLine}, fmt::format("{}\n", ClaudeLine));
				  });
			Entry& mcp = add(McpPath, FileKind::McpEntry);
			judge(mcp,
			      [&mcp, &evaluation, &project](FoundFile const& file)
			      {
					  JudgeMcp(mcp, file, evaluation.Tool, project.ProjectFile);
				  });
			Entry& skillEntry = add(SkillPath, FileKind::WholeFile);
			judge(skillEntry,
			      [&skillEntry, &skill, &evaluation](FoundFile const& file)
			      {
					  JudgeWholeFile(skillEntry, file, *skill, evaluation.Records);
				  });
			std::filesystem::path const docs = layout.GetContentPath(EngineContent::Docs);
			for (std::string_view const guide : AgentKit::Guides)
			{
				std::filesystem::path const sourcePath = docs.empty() ? docs : docs / FileSystem::PathFromUtf8(guide);
				std::optional<std::string> const source = ReadEngineFile(sourcePath);
				std::string const path = fmt::format("{}{}", GuidesDirectory, guide);
				if (!source)
				{
					evaluation.Warnings.push_back(fmt::format("the engine has no guide '{}', so '{}' is left out", guide, path));
					continue;
				}
				std::string const copy = RewriteGuideLinks(*source);
				Entry& guideEntry = add(path, FileKind::Guide);
				judge(guideEntry,
				      [&guideEntry, &copy, &evaluation](FoundFile const& file)
				      {
						  JudgeWholeFile(guideEntry, file, copy, evaluation.Records);
					  });
			}
			Entry& readme = add(GuidesReadmePath, FileKind::Guide);
			judge(readme,
			      [&readme, &evaluation](FoundFile const& file)
			      {
					  JudgeWholeFile(readme, file, std::string(GuidesReadmeText), evaluation.Records);
				  });
			if (options.GitIgnore)
			{
				Entry& gitIgnore = add(GitIgnorePath, FileKind::GitIgnore);
				judge(gitIgnore,
				      [&gitIgnore](FoundFile const& file)
				      {
						  JudgeLine(gitIgnore, file, GitIgnoreLine, {".strada/", ".strada", "/.strada/", "/.strada"}, GitIgnoreText);
					  });
			}

			for (size_t index = 0; index < evaluation.Entries.size(); index++)
			{
				Entry& entry = evaluation.Entries[index];
				entry.Action = GetAction(entry);
				if (entry.Action == AgentFileAction::SkipDamaged || entry.Action == AgentFileAction::SkipUserModified)
				{
					evaluation.Warnings.push_back(DescribeSkipped(entry, found[index]));
				}
			}

			Entry state;
			state.Path = std::string(AgentKit::StateFilePath);
			state.Kind = FileKind::State;
			JudgeState(state, stateFile,
			           MakeStateText(evaluation,
			                         [](Entry const&)
			                         {
										 return true;
									 }));
			state.Action = GetAction(state);
			if (state.Action == AgentFileAction::SkipDamaged)
			{
				evaluation.Warnings.push_back(DescribeSkipped(state, stateFile));
			}
			evaluation.Entries.push_back(std::move(state));
			return evaluation;
		}

		// A line of a diff, cut after AgentKit::MaxDiffLineBytes bytes at the start of a UTF-8 character.
		std::string CutDiffLine(std::string_view line)
		{
			if (line.size() <= AgentKit::MaxDiffLineBytes)
			{
				return std::string(line);
			}
			size_t cut = AgentKit::MaxDiffLineBytes;
			while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
			{
				cut--;
			}
			return fmt::format("{}... ({} more bytes)", line.substr(0, cut), line.size() - cut);
		}

		std::vector<std::string_view> SplitDiffLines(std::string_view text)
		{
			std::vector<std::string_view> lines;
			for (LineSpan const& line : SplitLines(text))
			{
				lines.push_back(text.substr(line.Begin, line.ContentEnd - line.Begin));
			}
			return lines;
		}

		// A unified-style diff of two texts without carriage returns: hunks with DiffContext lines of context, at most
		// AgentKit::MaxDiffLines lines and then one that says how many more there are.
		std::string MakeDiff(std::string_view before, std::string_view after)
		{
			if (before == after)
			{
				return {};
			}
			std::vector<std::string_view> const oldLines = SplitDiffLines(before);
			std::vector<std::string_view> const newLines = SplitDiffLines(after);
			size_t prefix = 0;
			while (prefix < oldLines.size() && prefix < newLines.size() && oldLines[prefix] == newLines[prefix])
			{
				prefix++;
			}
			size_t suffix = 0;
			while (suffix < oldLines.size() - prefix && suffix < newLines.size() - prefix &&
			       oldLines[oldLines.size() - 1 - suffix] == newLines[newLines.size() - 1 - suffix])
			{
				suffix++;
			}

			// The edit script: ' ' keeps a line of both, '-' removes one of the old text, '+' adds one of the new text.
			struct Edit
			{
				char Kind = ' ';
				std::string_view Line;
			};
			std::vector<Edit> edits;
			for (size_t index = 0; index < prefix; index++)
			{
				edits.push_back({' ', oldLines[index]});
			}
			size_t const oldCount = oldLines.size() - prefix - suffix;
			size_t const newCount = newLines.size() - prefix - suffix;
			if (oldCount > 0 && newCount > 0 && (oldCount + 1) * (newCount + 1) <= MaxDiffCells)
			{
				// Longest common subsequence of the middles, from their ends.
				size_t const width = newCount + 1;
				std::vector<uint32_t> common((oldCount + 1) * width, 0);
				for (size_t row = oldCount; row-- > 0;)
				{
					for (size_t column = newCount; column-- > 0;)
					{
						common[row * width + column] = oldLines[prefix + row] == newLines[prefix + column]
						                                   ? common[(row + 1) * width + column + 1] + 1
						                                   : std::max(common[(row + 1) * width + column], common[row * width + column + 1]);
					}
				}
				size_t row = 0;
				size_t column = 0;
				while (row < oldCount && column < newCount)
				{
					if (oldLines[prefix + row] == newLines[prefix + column])
					{
						edits.push_back({' ', oldLines[prefix + row]});
						row++;
						column++;
					}
					else if (common[(row + 1) * width + column] >= common[row * width + column + 1])
					{
						edits.push_back({'-', oldLines[prefix + row]});
						row++;
					}
					else
					{
						edits.push_back({'+', newLines[prefix + column]});
						column++;
					}
				}
				for (; row < oldCount; row++)
				{
					edits.push_back({'-', oldLines[prefix + row]});
				}
				for (; column < newCount; column++)
				{
					edits.push_back({'+', newLines[prefix + column]});
				}
			}
			else
			{
				for (size_t index = 0; index < oldCount; index++)
				{
					edits.push_back({'-', oldLines[prefix + index]});
				}
				for (size_t index = 0; index < newCount; index++)
				{
					edits.push_back({'+', newLines[prefix + index]});
				}
			}
			for (size_t index = oldLines.size() - suffix; index < oldLines.size(); index++)
			{
				edits.push_back({' ', oldLines[index]});
			}

			// The old and new line numbers (from 0) at each edit.
			std::vector<size_t> oldBefore(edits.size() + 1, 0);
			std::vector<size_t> newBefore(edits.size() + 1, 0);
			for (size_t index = 0; index < edits.size(); index++)
			{
				oldBefore[index + 1] = oldBefore[index] + (edits[index].Kind != '+' ? 1 : 0);
				newBefore[index + 1] = newBefore[index] + (edits[index].Kind != '-' ? 1 : 0);
			}

			std::vector<std::string> output;
			size_t index = 0;
			while (index < edits.size())
			{
				size_t change = index;
				while (change < edits.size() && edits[change].Kind == ' ')
				{
					change++;
				}
				if (change == edits.size())
				{
					break;
				}
				size_t const start = std::max(index, change >= DiffContext ? change - DiffContext : 0);
				size_t lastChange = change;
				size_t scan = change;
				while (scan < edits.size())
				{
					if (edits[scan].Kind != ' ')
					{
						lastChange = scan;
					}
					else if (scan - lastChange > 2 * DiffContext)
					{
						break;
					}
					scan++;
				}
				size_t const end = std::min(edits.size(), lastChange + DiffContext + 1);
				size_t const oldLength = oldBefore[end] - oldBefore[start];
				size_t const newLength = newBefore[end] - newBefore[start];
				output.push_back(fmt::format("@@ -{},{} +{},{} @@", oldLength > 0 ? oldBefore[start] + 1 : oldBefore[start], oldLength,
				                             newLength > 0 ? newBefore[start] + 1 : newBefore[start], newLength));
				for (size_t line = start; line < end; line++)
				{
					output.push_back(fmt::format("{}{}", edits[line].Kind, CutDiffLine(edits[line].Line)));
				}
				index = end;
			}

			std::string diff;
			size_t const shown = std::min(output.size(), AgentKit::MaxDiffLines);
			for (size_t line = 0; line < shown; line++)
			{
				diff += output[line];
				diff += '\n';
			}
			if (output.size() > shown)
			{
				diff += fmt::format("... {} more lines\n", output.size() - shown);
			}
			return diff;
		}

		AgentPlan MakePlan(Evaluation const& evaluation)
		{
			AgentPlan plan;
			for (Entry const& entry : evaluation.Entries)
			{
				AgentFilePlan& file = plan.Files.emplace_back();
				file.Path = entry.Path;
				file.Action = entry.Action;
				if (Writes(entry.Action))
				{
					file.Diff = MakeDiff(entry.OldPart, entry.NewPart);
				}
			}
			plan.Warnings = evaluation.Warnings;
			return plan;
		}

		Result<void> WriteProjectFile(std::filesystem::path const& projectDirectory, std::string const& relative, std::string const& text)
		{
			// The plan read the file through no symbolic link; reading again catches one made since.
			FoundFile const file = ReadProjectFile(projectDirectory, relative);
			if (file.Exists && !file.Problem.empty())
			{
				return Error{file.Problem};
			}
			std::filesystem::path const path = projectDirectory / FileSystem::PathFromUtf8(relative);
			if (FileSystem::IsReadOnly(path))
			{
				return Error{"the file is read-only"};
			}
			return FileSystem::WriteTextFile(path, text);
		}
	}

	std::string_view GetAgentFileStateName(AgentFileState state)
	{
		switch (state)
		{
			case AgentFileState::Missing:
				return "Missing";
			case AgentFileState::Current:
				return "Current";
			case AgentFileState::Outdated:
				return "Outdated";
			case AgentFileState::UserModified:
				return "UserModified";
			case AgentFileState::Damaged:
				return "Damaged";
		}
		return "Damaged";
	}

	std::string_view GetAgentFileActionName(AgentFileAction action)
	{
		switch (action)
		{
			case AgentFileAction::Create:
				return "Create";
			case AgentFileAction::Update:
				return "Update";
			case AgentFileAction::Unchanged:
				return "Unchanged";
			case AgentFileAction::SkipUserModified:
				return "SkipUserModified";
			case AgentFileAction::SkipDamaged:
				return "SkipDamaged";
		}
		return "SkipDamaged";
	}

	std::vector<std::string> AgentKit::ListFiles(EngineLayout const& layout, AgentSetupOptions const& options)
	{
		std::vector<std::string> files = {std::string(AgentsPath), std::string(ClaudePath), std::string(McpPath), std::string(SkillPath)};
		std::filesystem::path const docs = layout.GetContentPath(EngineContent::Docs);
		for (std::string_view const guide : Guides)
		{
			if (!docs.empty() && FileSystem::IsRegularFile(docs / FileSystem::PathFromUtf8(guide)))
			{
				files.push_back(fmt::format("{}{}", GuidesDirectory, guide));
			}
		}
		files.emplace_back(GuidesReadmePath);
		if (options.GitIgnore)
		{
			files.emplace_back(GitIgnorePath);
		}
		files.emplace_back(StateFilePath);
		return files;
	}

	Result<AgentStatus> AgentKit::GetStatus(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool,
	                                        AgentSetupOptions const& options)
	{
		Result<Evaluation> evaluation = Evaluate(project, layout, findTool, options);
		if (!evaluation)
		{
			return Error{evaluation.GetError()};
		}
		AgentStatus status;
		for (Entry const& entry : evaluation.GetValue().Entries)
		{
			status.Files.push_back(AgentFileStatus{entry.Path, entry.State});
		}
		status.Tool = evaluation.GetValue().Tool;
		status.Warnings = evaluation.GetValue().Warnings;
		return status;
	}

	Result<AgentPlan> AgentKit::Plan(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool,
	                                 AgentSetupOptions const& options)
	{
		Result<Evaluation> evaluation = Evaluate(project, layout, findTool, options);
		if (!evaluation)
		{
			return Error{evaluation.GetError()};
		}
		return MakePlan(evaluation.GetValue());
	}

	AgentApplyResult AgentKit::Apply(AgentProject const& project, EngineLayout const& layout, AgentToolLookup const& findTool,
	                                 AgentSetupOptions const& options)
	{
		AgentApplyResult result;
		Result<Evaluation> evaluated = Evaluate(project, layout, findTool, options);
		if (!evaluated)
		{
			result.Error = evaluated.GetError();
			return result;
		}
		Evaluation const& evaluation = evaluated.GetValue();
		AgentPlan const plan = MakePlan(evaluation);
		result.Plan.Warnings = plan.Warnings;
		// The entries the loop wrote, by index; the state file comes last.
		size_t const stateIndex = evaluation.Entries.size() - 1;
		size_t written = 0;
		for (; written < stateIndex; written++)
		{
			Entry const& entry = evaluation.Entries[written];
			if (Writes(entry.Action))
			{
				if (Result<void> wrote = WriteProjectFile(project.Directory, entry.Path, entry.NewText); !wrote)
				{
					result.FailedPath = entry.Path;
					result.Error = fmt::format("'{}' cannot be written: {}", entry.Path, wrote.GetError());
					break;
				}
			}
			result.Plan.Files.push_back(plan.Files[written]);
		}

		Entry const& state = evaluation.Entries[stateIndex];
		if (result.Succeeded())
		{
			if (Writes(state.Action))
			{
				if (Result<void> wrote = WriteProjectFile(project.Directory, state.Path, state.NewText); !wrote)
				{
					result.FailedPath = state.Path;
					result.Error = fmt::format("'{}' cannot be written: {}", state.Path, wrote.GetError());
					return result;
				}
			}
			result.Plan.Files.push_back(plan.Files[stateIndex]);
			return result;
		}

		// Stopped: the state file records what was written, so the files stay Outdated rather than UserModified.
		if (state.Action != AgentFileAction::SkipDamaged)
		{
			std::string const text = MakeStateText(evaluation,
			                                       [&evaluation, written](Entry const& candidate)
			                                       {
													   return static_cast<size_t>(&candidate - evaluation.Entries.data()) < written;
												   });
			FoundFile const current = ReadProjectFile(project.Directory, StateFilePath);
			if (!current.Exists || RemoveCarriageReturns(current.Text) != text)
			{
				if (Result<void> wrote = WriteProjectFile(project.Directory, std::string(StateFilePath), text); !wrote)
				{
					result.Plan.Warnings.push_back(fmt::format("'{}' cannot be written: {}", StateFilePath, wrote.GetError()));
				}
			}
		}
		return result;
	}

	std::string AgentKit::ResolveTool(EngineLayout const& layout, AgentToolLookup const& findTool, std::vector<std::string>* warnings)
	{
		std::filesystem::path const tool = layout.GetToolPath();
		if (tool.empty() || !FileSystem::IsRegularFile(tool))
		{
			if (warnings != nullptr)
			{
				warnings->emplace_back("strada is not beside the editor: put it on PATH, as .mcp.json runs it by its name");
			}
			return "strada";
		}
		if (findTool)
		{
			if (std::optional<std::filesystem::path> const found = findTool())
			{
				std::error_code error;
				if (std::filesystem::equivalent(*found, tool, error) && !error)
				{
					return "strada";
				}
			}
		}
		std::error_code error;
		std::filesystem::path absolute = std::filesystem::absolute(tool, error);
		if (error)
		{
			absolute = tool;
		}
		std::string const path = FileSystem::PathToUtf8(absolute.lexically_normal());
		if (warnings != nullptr)
		{
			warnings->push_back(fmt::format("no strada on PATH is the one beside the editor, so .mcp.json names '{}', a path of this "
			                                "machine: put the editor's folder on PATH to keep the file portable",
			                                path));
		}
		return path;
	}
}
