#include "Game.hpp"

#include "gl_errors.hpp" //helper for dumpping OpenGL error messages
#include "read_chunk.hpp" //helper for reading a vector of structures from a file
#include "data_path.hpp" //helper to get paths relative to executable

#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <fstream>
#include <map>
#include <cstddef>
//#include <random>

//helper defined later; throws if shader compilation fails:
static GLuint compile_shader(GLenum type, std::string const &source);

Game::Game() {
	{ //create an opengl program to perform sun/sky (well, directional+hemispherical) lighting:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 object_to_clip;\n"
			"uniform mat4x3 object_to_light;\n"
			"uniform mat3 normal_to_light;\n"
			"layout(location=0) in vec4 Position;\n" //note: layout keyword used to make sure that the location-0 attribute is always bound to something
			"in vec3 Normal;\n"
			"in vec4 Color;\n"
			"out vec3 position;\n"
			"out vec3 normal;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"	gl_Position = object_to_clip * Position;\n"
			"	position = object_to_light * Position;\n"
			"	normal = normal_to_light * Normal;\n"
			"	color = Color;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 sun_direction;\n"
			"uniform vec3 sun_color;\n"
			"uniform vec3 sky_direction;\n"
			"uniform vec3 sky_color;\n"
			"in vec3 position;\n"
			"in vec3 normal;\n"
			"in vec4 color;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	vec3 total_light = vec3(0.0, 0.0, 0.0);\n"
			"	vec3 n = normalize(normal);\n"
			"	{ //sky (hemisphere) light:\n"
			"		vec3 l = sky_direction;\n"
			"		float nl = 0.5 + 0.5 * dot(n,l);\n"
			"		total_light += nl * sky_color;\n"
			"	}\n"
			"	{ //sun (directional) light:\n"
			"		vec3 l = sun_direction;\n"
			"		float nl = max(0.0, dot(n,l));\n"
			"		total_light += nl * sun_color;\n"
			"	}\n"
			"	fragColor = vec4(color.rgb * total_light, color.a);\n"
			"}\n"
		);

		simple_shading.program = glCreateProgram();
		glAttachShader(simple_shading.program, vertex_shader);
		glAttachShader(simple_shading.program, fragment_shader);
		//shaders are reference counted so this makes sure they are freed after program is deleted:
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//link the shader program and throw errors if linking fails:
		glLinkProgram(simple_shading.program);
		GLint link_status = GL_FALSE;
		glGetProgramiv(simple_shading.program, GL_LINK_STATUS, &link_status);
		if (link_status != GL_TRUE) {
			std::cerr << "Failed to link shader program." << std::endl;
			GLint info_log_length = 0;
			glGetProgramiv(simple_shading.program, GL_INFO_LOG_LENGTH, &info_log_length);
			std::vector< GLchar > info_log(info_log_length, 0);
			GLsizei length = 0;
			glGetProgramInfoLog(simple_shading.program, GLsizei(info_log.size()), &length, &info_log[0]);
			std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
			throw std::runtime_error("failed to link program");
		}
	}

	{ //read back uniform and attribute locations from the shader program:
		simple_shading.object_to_clip_mat4 = glGetUniformLocation(simple_shading.program, "object_to_clip");
		simple_shading.object_to_light_mat4x3 = glGetUniformLocation(simple_shading.program, "object_to_light");
		simple_shading.normal_to_light_mat3 = glGetUniformLocation(simple_shading.program, "normal_to_light");

		simple_shading.sun_direction_vec3 = glGetUniformLocation(simple_shading.program, "sun_direction");
		simple_shading.sun_color_vec3 = glGetUniformLocation(simple_shading.program, "sun_color");
		simple_shading.sky_direction_vec3 = glGetUniformLocation(simple_shading.program, "sky_direction");
		simple_shading.sky_color_vec3 = glGetUniformLocation(simple_shading.program, "sky_color");

		simple_shading.Position_vec4 = glGetAttribLocation(simple_shading.program, "Position");
		simple_shading.Normal_vec3 = glGetAttribLocation(simple_shading.program, "Normal");
		simple_shading.Color_vec4 = glGetAttribLocation(simple_shading.program, "Color");
	}

	struct Vertex {
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 28, "Vertex should be packed.");

	{ //load mesh data from a binary blob:
		std::ifstream blob(data_path("meshes.blob"), std::ios::binary);
		//The blob will be made up of three chunks:
		// the first chunk will be vertex data (interleaved position/normal/color)
		// the second chunk will be characters
		// the third chunk will be an index, mapping a name (range of characters) to a mesh (range of vertex data)

		//read vertex data:
		std::vector< Vertex > vertices;
		read_chunk(blob, "dat0", &vertices);

		//read character data (for names):
		std::vector< char > names;
		read_chunk(blob, "str0", &names);

		//read index:
		struct IndexEntry {
			uint32_t name_begin;
			uint32_t name_end;
			uint32_t vertex_begin;
			uint32_t vertex_end;
		};
		static_assert(sizeof(IndexEntry) == 16, "IndexEntry should be packed.");

		std::vector< IndexEntry > index_entries;
		read_chunk(blob, "idx0", &index_entries);

		if (blob.peek() != EOF) {
			std::cerr << "WARNING: trailing data in meshes file." << std::endl;
		}

		//upload vertex data to the graphics card:
		glGenBuffers(1, &meshes_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		//create map to store index entries:
		std::map< std::string, Mesh > index;
		for (IndexEntry const &e : index_entries) {
			if (e.name_begin > e.name_end || e.name_end > names.size()) {
				throw std::runtime_error("invalid name indices in index.");
			}
			if (e.vertex_begin > e.vertex_end || e.vertex_end > vertices.size()) {
				throw std::runtime_error("invalid vertex indices in index.");
			}
			Mesh mesh;
			mesh.first = e.vertex_begin;
			mesh.count = e.vertex_end - e.vertex_begin;
			auto ret = index.insert(std::make_pair(
				std::string(names.begin() + e.name_begin, names.begin() + e.name_end),
				mesh));
			if (!ret.second) {
				throw std::runtime_error("duplicate name in index.");
			}
		}

		//look up into index map to extract meshes:
		auto lookup = [&index](std::string const &name) -> Mesh {
			auto f = index.find(name);
			if (f == index.end()) {
				throw std::runtime_error("Mesh named '" + name + "' does not appear in index.");
			}
			return f->second;
		};
		tile_mesh = lookup("Tile");
        blackpiece_mesh = lookup("blackpiece");  // I Change the loading target
        whitepiece_mesh = lookup("whitepiece");
	}

	{ //create vertex array object to hold the map from the mesh vertex buffer to shader program attributes:
		glGenVertexArrays(1, &meshes_for_simple_shading_vao);
		glBindVertexArray(meshes_for_simple_shading_vao);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		//note that I'm specifying a 3-vector for a 4-vector attribute here, and this is okay to do:
		glVertexAttribPointer(simple_shading.Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
		glEnableVertexAttribArray(simple_shading.Position_vec4);
		if (simple_shading.Normal_vec3 != -1U) {
			glVertexAttribPointer(simple_shading.Normal_vec3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Normal));
			glEnableVertexAttribArray(simple_shading.Normal_vec3);
		}
		if (simple_shading.Color_vec4 != -1U) {
			glVertexAttribPointer(simple_shading.Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
			glEnableVertexAttribArray(simple_shading.Color_vec4);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GL_ERRORS();

	//----------------
	//set up game board with meshes and rolls:
    uint32_t numel = board_size.x * board_size.y;
	board_rotations.reserve(numel);
    whitepieces.reserve(numel);
    blackpieces.reserve(numel);

	for (uint32_t i = 0; i < board_size.x * board_size.y; ++i) {
		board_rotations.emplace_back(glm::quat());
	}

    generate_new_stage();
}

Game::~Game() {
	glDeleteVertexArrays(1, &meshes_for_simple_shading_vao);
	meshes_for_simple_shading_vao = -1U;

	glDeleteBuffers(1, &meshes_vbo);
	meshes_vbo = -1U;

	glDeleteProgram(simple_shading.program);
	simple_shading.program = -1U;

	GL_ERRORS();
}

bool Game::handle_event(SDL_Event const &evt, glm::uvec2 window_size) {
	//ignore any keys that are the result of automatic key repeat:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat) {
		return false;
	}

	//handling arrow keys and R press:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat == 0) {
        //press R to generate a new game
        if (evt.key.keysym.scancode == SDL_SCANCODE_R) {
            generate_new_stage();
            return true;
        }

        {  //powerful slide (remove duplicate objects in the same row/column)
            if (evt.key.keysym.mod == KMOD_LSHIFT || evt.key.keysym.mod == KMOD_RSHIFT) {
                auto checkPiece = [] (Piece& prevPiece, uint32_t row, uint32_t column) {
                    if (board_state[row][column] == prevPiece) {
                        board_state[row][column] = Empty;
                    } else if (board_state[row][column] != Empty) {
                        prevPiece = board_state[row][column];
                    }
                };
                if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT ||
                    evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {  //horizontal powerful slide
                    for (uint32_t row = 0; row < board_size.y; ++row) {
                        uint32_t column = 0;
                        Piece prevPiece = Empty;
                        while (prevPiece == Empty && column < board_size.x) {  //find first non-Empty piece
                            prevPiece = board_state[row][column++];
                        }
                        for (; column < board_size.x; ++column) {
                            checkPiece(prevPiece, row, column);
                        }
                    }
                } else if (evt.key.keysym.scancode == SDL_SCANCODE_UP ||
                           evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {  //vertical powerful slide
                    for (uint32_t column = 0; column < board_size.x; ++column) {
                        uint32_t row = 0;
                        Piece prevPiece = Empty;
                        while (prevPiece == Empty && row < board_size.y) {  //find first non-Empty piece
                            prevPiece = board_state[row++][column];
                        }
                        for (; row < board_size.y; ++row) {
                            checkPiece(prevPiece, row, column);
                        }
                    }
                }
            }
        }

        {  //slide
            //for directions should be handled in four different indexing
            if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
                for (uint32_t r = 0; r < board_size.y; ++r) {
                    uint32_t head = 0;
                    while (board_state[r][head] != Empty && head < board_size.x) {  //find first Empty position
                        ++head;
                    }
                    for (uint32_t c = head + 1; c < board_size.x; ++c) {
                        if (board_state[r][c] != Empty && c != head && board_state[r][head] == Empty) {
                            std::swap(board_state[r][head++], board_state[r][c]);
                        }
                    }
                }
                return true;
            } else if (evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
                for (uint32_t r = board_size.y; r-- > 0; ) {  //unsigned reverse traverse
                    uint32_t head = board_size.x - 1;
                    while (board_state[r][head] != Empty && head > 0) {
                        --head;
                    }
                    for (uint32_t c = head; c-- > 0; ) {
                        if (board_state[r][c] != Empty && c != head && board_state[r][head] == Empty) {
                            std::swap(board_state[r][head--], board_state[r][c]);
                        }
                    }
                }
                return true;
            } else if (evt.key.keysym.scancode == SDL_SCANCODE_UP) {
                for (uint32_t c = 0; c < board_size.x; ++c) {
                    uint32_t head = 0;
                    while (board_state[head][c] != Empty && head < board_size.y) {
                        ++head;
                    }
                    for (uint32_t r = head + 1; r < board_size.y; ++r) {
                        if (board_state[r][c] != Empty && r != head && board_state[head][c] == Empty) {
                            std::swap(board_state[head++][c], board_state[r][c]);
                        }
                    }
                }
                return true;
            } else if (evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
                for (uint32_t c = board_size.x; c-- > 0; ) {
                    uint32_t head = board_size.y - 1;
                    while (board_state[head][c] != Empty && head > 0) {
                        --head;
                    }
                    for (uint32_t r = head; r-- > 0; ) {
                        if (board_state[r][c] != Empty && r != head && board_state[head][c] == Empty) {
                            std::swap(board_state[head--][c], board_state[r][c]);
                        }
                    }
                }
                return true;
            }
        }
	}
	return false;
}

void Game::update(float elapsed) {
    //update positions of black/white pieces based on board_state
    uint32_t blackIdx = 0;
    uint32_t whiteIdx = 0;
    for (uint32_t row = 0; row < board_size.y; ++row) {
        for (uint32_t column = 0; column < board_size.x; ++column) {
            if (board_state[row][column] == Black) {  // blackpieces
                blackpieces[blackIdx].x = column;
                blackpieces[blackIdx].y = board_size.y - 1 - row;
                ++blackIdx;
            } else if (board_state[row][column] == White) {  // whitepieces
                whitepieces[whiteIdx].x = column;
                whitepieces[whiteIdx].y = board_size.y - 1 - row;
                ++whiteIdx;
            }
        }
    }
    blackpieces.resize(blackIdx);
    whitepieces.resize(whiteIdx);

    // check if player wins
    if (blackpieces.size() == 1 && whitepieces.size() == 1) {
        game_state = Win;
    }
}

void Game::draw(glm::uvec2 drawable_size) {
	//Set up a transformation matrix to fit the board in the window:
	glm::mat4 world_to_clip;
	{
		float aspect = float(drawable_size.x) / float(drawable_size.y);

		//want scale such that board * scale fits in [-aspect,aspect]x[-1.0,1.0] screen box:
		float scale = glm::min(
			2.0f * aspect / float(board_size.x),
			2.0f / float(board_size.y)
		);

		//center of board will be placed at center of screen:
		glm::vec2 center = 0.5f * glm::vec2(board_size);

		//NOTE: glm matrices are specified in column-major order
		world_to_clip = glm::mat4(
			scale / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f,-1.0f, 0.0f,
			-(scale / aspect) * center.x, -scale * center.y, 0.0f, 1.0f
		);
	}

	//set up graphics pipeline to use data from the meshes and the simple shading program:
	glBindVertexArray(meshes_for_simple_shading_vao);
	glUseProgram(simple_shading.program);

	glUniform3fv(simple_shading.sun_color_vec3, 1, glm::value_ptr(glm::vec3(0.81f, 0.81f, 0.76f)));
	glUniform3fv(simple_shading.sun_direction_vec3, 1, glm::value_ptr(glm::normalize(glm::vec3(-0.2f, 0.2f, 1.0f))));
	glUniform3fv(simple_shading.sky_color_vec3, 1, glm::value_ptr(glm::vec3(0.2f, 0.2f, 0.3f)));
	glUniform3fv(simple_shading.sky_direction_vec3, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));

	//helper function to draw a given mesh with a given transformation:
	auto draw_mesh = [&](Mesh const &mesh, glm::mat4 const &object_to_world) {
		//set up the matrix uniforms:
		if (simple_shading.object_to_clip_mat4 != -1U) {
			glm::mat4 object_to_clip = world_to_clip * object_to_world;
			glUniformMatrix4fv(simple_shading.object_to_clip_mat4, 1, GL_FALSE, glm::value_ptr(object_to_clip));
		}
		if (simple_shading.object_to_light_mat4x3 != -1U) {
			glUniformMatrix4x3fv(simple_shading.object_to_light_mat4x3, 1, GL_FALSE, glm::value_ptr(object_to_world));
		}
		if (simple_shading.normal_to_light_mat3 != -1U) {
			//NOTE: if there isn't any non-uniform scaling in the object_to_world matrix, then the inverse transpose is the matrix itself, and computing it wastes some CPU time:
			glm::mat3 normal_to_world = glm::inverse(glm::transpose(glm::mat3(object_to_world)));
			glUniformMatrix3fv(simple_shading.normal_to_light_mat3, 1, GL_FALSE, glm::value_ptr(normal_to_world));
		}

		//draw the mesh:
		glDrawArrays(GL_TRIANGLES, mesh.first, mesh.count);
	};

	for (uint32_t y = 0; y < board_size.y; ++y) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			draw_mesh(tile_mesh,
				glm::mat4(
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					x+0.5f, y+0.5f,-0.5f, 1.0f
				)
			);
		}
	}
    for (auto& b : blackpieces) {
        draw_mesh(blackpiece_mesh,
            glm::mat4(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                b.x+0.5f, b.y+0.5f, 0.0f, 1.0f
            )
        );
    }
    for (auto& w : whitepieces) {
        draw_mesh(whitepiece_mesh,
            glm::mat4(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                w.x+0.5f, w.y+0.5f, 0.0f, 1.0f
            )
        );
    }


	glUseProgram(0);

	GL_ERRORS();
}

void Game::generate_new_stage() {
    { //set positions of pieces
        blackpieces.clear();
        whitepieces.clear();
        for (uint32_t r = 0; r < board_size.y; ++r) {
            for (uint32_t c = 0; c < board_size.x; ++c) {
                auto piece = mt() % 3;  //Empty, Black, White
                if (piece == White) {
                    whitepieces.emplace_back(glm::vec2(r,c));
                } else if (piece == Black) {
                    blackpieces.emplace_back(glm::vec2(r,c));
                }
            }
        }
    }

    { //set board_state
        memset(board_state, Empty, sizeof(board_state));  //zero-out the board_state
        auto set_board_state = [&] (std::vector< glm::uvec2 >& piece_pos, Piece piece) {
            for (auto& p : piece_pos) {
                board_state[board_size.y - 1 - p.y][p.x] = piece;
            }
        };
        set_board_state(blackpieces, Black);
        set_board_state(whitepieces, White);
    }

    { //set game_state
        game_state = GoOn;
    }

    //re-generate the game if current board is invalid
    if (whitepieces.size() < 1 || blackpieces.size() < 1) {
        generate_new_stage();
    }
}


//create and return an OpenGL vertex shader from source:
static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, GLsizei(info_log.size()), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}
