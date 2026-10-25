#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Utils.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "libvgcode/include/Types.hpp"
#include <glad/gl.h>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {
// libvgcode owns its render passes; preserve the host canvas bindings as well
// as the framebuffer and viewport while a separate viewer is alive.
struct SliceGlState {
    GLint draw_fbo=0,read_fbo=0,renderbuffer=0,program=0,vao=0,array_buffer=0,texture_buffer=0,active_texture=0;
    GLint pack_buffer=0,unpack_buffer=0,pack_alignment=0,unpack_alignment=0,depth_func=0,pack_row_length=0,pack_skip_pixels=0,pack_skip_rows=0;
    GLint blend_src_rgb=0,blend_dst_rgb=0,blend_src_alpha=0,blend_dst_alpha=0,blend_rgb=0,blend_alpha=0;
    std::array<GLint,4> viewport{},scissor{};
    std::array<GLfloat,4> clear_color{};
    std::array<GLboolean,4> color_mask{};
    GLdouble clear_depth=1.0;
    GLboolean depth_mask=GL_TRUE;
    bool depth=false,blend=false,cull=false,scissor_test=false;
    std::array<GLint,8> textures{},texture_buffers{};
    SliceGlState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fbo);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);glGetIntegerv(GL_CURRENT_PROGRAM,&program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array_buffer);glGetIntegerv(GL_TEXTURE_BUFFER_BINDING,&texture_buffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack_buffer);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack_buffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH,&pack_row_length);glGetIntegerv(GL_PACK_SKIP_PIXELS,&pack_skip_pixels);glGetIntegerv(GL_PACK_SKIP_ROWS,&pack_skip_rows);glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clear_depth);
        glGetIntegerv(GL_PACK_ALIGNMENT,&pack_alignment);glGetIntegerv(GL_UNPACK_ALIGNMENT,&unpack_alignment);
        glGetIntegerv(GL_VIEWPORT,viewport.data());glGetIntegerv(GL_SCISSOR_BOX,scissor.data());glGetFloatv(GL_COLOR_CLEAR_VALUE,clear_color.data());
        glGetBooleanv(GL_COLOR_WRITEMASK,color_mask.data());glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);glGetIntegerv(GL_DEPTH_FUNC,&depth_func);
        glGetIntegerv(GL_BLEND_SRC_RGB,&blend_src_rgb);glGetIntegerv(GL_BLEND_DST_RGB,&blend_dst_rgb);glGetIntegerv(GL_BLEND_SRC_ALPHA,&blend_src_alpha);glGetIntegerv(GL_BLEND_DST_ALPHA,&blend_dst_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB,&blend_rgb);glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&blend_alpha);
        depth=glIsEnabled(GL_DEPTH_TEST);blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);scissor_test=glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&active_texture);
        for(unsigned int i=0;i<textures.size();++i){glActiveTexture(GL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&textures[i]);glGetIntegerv(GL_TEXTURE_BINDING_BUFFER,&texture_buffers[i]);}
        glActiveTexture(active_texture);
    }
    ~SliceGlState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo);glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
        glUseProgram(program);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,array_buffer);glBindBuffer(GL_TEXTURE_BUFFER,texture_buffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,pack_buffer);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack_buffer);
        glPixelStorei(GL_PACK_ROW_LENGTH,pack_row_length);glPixelStorei(GL_PACK_SKIP_PIXELS,pack_skip_pixels);glPixelStorei(GL_PACK_SKIP_ROWS,pack_skip_rows);glClearDepth(clear_depth);
        glPixelStorei(GL_PACK_ALIGNMENT,pack_alignment);glPixelStorei(GL_UNPACK_ALIGNMENT,unpack_alignment);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);glClearColor(clear_color[0],clear_color[1],clear_color[2],clear_color[3]);
        glColorMask(color_mask[0],color_mask[1],color_mask[2],color_mask[3]);glDepthMask(depth_mask);glDepthFunc(depth_func);
        glBlendFuncSeparate(blend_src_rgb,blend_dst_rgb,blend_src_alpha,blend_dst_alpha);glBlendEquationSeparate(blend_rgb,blend_alpha);
        for(auto entry:{std::pair<GLenum,bool>{GL_DEPTH_TEST,depth},{GL_BLEND,blend},{GL_CULL_FACE,cull},{GL_SCISSOR_TEST,scissor_test}}){if(entry.second)glEnable(entry.first);else glDisable(entry.first);}
        for(unsigned int i=0;i<textures.size();++i){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,textures[i]);glBindTexture(GL_TEXTURE_BUFFER,texture_buffers[i]);}
        glActiveTexture(active_texture);
    }
};
}

bool GLCanvas3D::render_slice_thumbnail(ThumbnailData& data,const GCodeProcessorResult& result,
    const std::vector<std::string>& colors,unsigned int width,unsigned int height,Camera::ViewAngleType angle)
{
    data.reset();
    if(!m_initialized||!_set_shown_canvas_current()||OpenGLManager::get_framebuffers_type()!=OpenGLManager::EFramebufferType::Arb)
        return false;
    if(width<64||height<64||width>1024||height>1024||result.moves.empty()||result.moves.size()>2000000)
        return false;
    const SliceGlState saved;
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    libvgcode::Viewer viewer;
    const char* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if(!version)return false;
    const ScopeGuard stop([&viewer](){viewer.shutdown();});
    viewer.init(version);
    viewer.reset_default_extrusion_roles_colors();
    viewer.load(libvgcode::convert(result,colors,{},viewer));
    if(viewer.get_extrusion_roles().empty())return false;
    viewer.set_view_type(libvgcode::EViewType::Tool);
    const auto box=viewer.get_extrusion_bounding_box();
    const BoundingBoxf3 bounds(libvgcode::convert(box[0]).cast<double>(),libvgcode::convert(box[1]).cast<double>());
    if(!bounds.defined||!bounds.min.allFinite()||!bounds.max.allFinite()||bounds.size().norm()<=0)return false;
    Camera camera;camera.set_type(Camera::EType::Ortho);camera.set_scene_box(bounds);camera.set_viewport(0,0,width,height);camera.select_view(angle);camera.zoom_to_box(bounds);camera.apply_projection(bounds);
    GLuint framebuffer=0,color=0,depth=0;
    glGenFramebuffers(1,&framebuffer);glGenRenderbuffers(1,&color);glGenRenderbuffers(1,&depth);
    const ScopeGuard release([&](){glDeleteRenderbuffers(1,&depth);glDeleteRenderbuffers(1,&color);glDeleteFramebuffers(1,&framebuffer);});
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER,color);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,width,height);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,color);
    glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,width,height);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    const GLenum buffer=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&buffer);glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
    glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);glDisable(GL_BLEND);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearDepth(1.0);glClearColor(0.12f,0.14f,0.17f,1.0f);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    const auto view=libvgcode::convert(static_cast<Matrix4f>(camera.get_view_matrix().matrix().cast<float>()));
    const auto projection=libvgcode::convert(static_cast<Matrix4f>(camera.get_projection_matrix().matrix().cast<float>()));
    viewer.render(view,projection);viewer.render_translucent(view,projection);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);glReadBuffer(GL_COLOR_ATTACHMENT0);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);
    data.set(width,height);if(!data.is_valid())return false;
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,data.pixels.data());
    return true;
}
} // namespace Slic3r::GUI
